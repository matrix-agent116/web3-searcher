////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "indexer/table.h"

#include <absl/cleanup/cleanup.h>

#include <algorithm>
#include <cmath>
#include <duckdb/common/vector/flat_vector.hpp>
#include <fstream>
#include <iresearch/formats/column/col_reader.hpp>
#include <iresearch/index/file_names.hpp>
#include <iresearch/index/segment_list.hpp>
#include <iresearch/index/segment_reader_impl.hpp>
#include <iresearch/search/filters/term_filter.hpp>
#include <iresearch/store/mmap_directory.hpp>
#include <iresearch/utils/directory_utils.hpp>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "common/http.h"
#include "indexer/directory_lock.h"
#include "indexer/schema.h"
#include "indexer/update_log.h"

namespace web3 {
namespace fs = std::filesystem;

namespace {

std::string ReadManifest(const fs::path& path) {
  std::ifstream in(path / "segments");
  if (!in)
    throw std::runtime_error("missing index manifest for table: " +
                             path.string());
  return {std::istreambuf_iterator<char>(in), {}};
}

void WriteManifest(irs::Directory& dir, std::string_view name,
                   std::string_view manifest) {
  auto out = dir.create(name);
  auto bytes = irs::ViewCast<irs::byte_type>(manifest);
  out->WriteData(bytes.data(), bytes.size());
  out->Close();
}

std::string WriteSegments(irs::Directory& dir,
                          duckdb::DatabaseInstance& database,
                          const TableSchema& schema,
                          simdjson::dom::array documents, uint64_t& last_id,
                          const UpdateLog* log = nullptr,
                          std::span<const uint8_t> buffered_modes = {},
                          std::span<const uint8_t> modes = {}) {
  irs::IndexWriterOptions options;
  options.db = &database;
  options.reader_options.db = &database;
  auto writer = irs::IndexWriter::Make(dir, std::move(options));
  writer->ReserveSegmentId(last_id);
  auto batch = writer->GetBatch();
  const bool mark_rows = std::ranges::contains(buffered_modes, 1) ||
                         std::ranges::contains(modes, 1);
  auto insert = [&](simdjson::dom::array rows,
                    std::span<const uint8_t> decisions) {
    size_t i = 0;
    for (auto row : rows) {
      InsertRow(batch, schema, row.get_object().value(),
                mark_rows ? std::optional<bool>(decisions[i]) : std::nullopt);
      ++i;
    }
  };
  if (log) {
    log->Replay([&](simdjson::dom::array rows) {
      if (rows.size() > buffered_modes.size())
        throw std::runtime_error("WAL row count changed");
      insert(rows, buffered_modes.first(rows.size()));
      buffered_modes = buffered_modes.subspan(rows.size());
    });
    if (!buffered_modes.empty())
      throw std::runtime_error("incomplete update log");
  }
  insert(documents, modes);
  std::string manifest;
  for (const auto& segment : batch.FlushSegments(true)) {
    uint64_t id;
    if (!irs::ParseSegmentId(segment.meta.name, id)) {
      throw std::runtime_error("invalid segment name");
    }
    last_id = std::max(last_id, id);
    irs::AppendSegmentList(manifest, id, segment.meta.kinds,
                           segment.meta.docs_count);
  }
  return manifest;
}

duckdb::Value ReadColumn(const irs::SubReader& segment, irs::doc_id_t id,
                         irs::field_id field, const duckdb::LogicalType& type) {
  auto* columns = segment.GetColReader();
  auto* column = columns ? columns->Column(field) : nullptr;
  if (!column)
    throw std::runtime_error("missing column: " + std::to_string(field));
  irs::ReadContext context{*columns};
  auto scan = column->InitScan(context);
  auto offset = id - irs::doc_limits::min();
  if (offset) column->Skip(scan, offset);
  duckdb::Vector result(type, 1);
  column->Scan(scan, result, 1);
  return result.GetValue(0);
}

bool SameValue(const duckdb::Value& left, const duckdb::Value& right) {
  // Indexed terms use ToString(), including the spelling of signed zero.
  return duckdb::Value::NotDistinctFrom(left, right) &&
         left.ToString() == right.ToString();
}

std::string JsonValue(const duckdb::Value& value) {
  using duckdb::LogicalTypeId;
  if (value.IsNull()) {
    return "null";
  }
  const auto& type = value.type();
  if (type.id() == LogicalTypeId::STRUCT) {
    std::string object = "{";
    const auto& members = duckdb::StructType::GetChildTypes(type);
    const auto& values = duckdb::StructValue::GetChildren(value);
    for (size_t i = 0; i < members.size(); ++i) {
      if (i) {
        object += ',';
      }
      object += Quote(members[i].first.GetIdentifierName()) + ":" +
                JsonValue(values[i]);
    }
    return object + "}";
  }
  if (type.id() == LogicalTypeId::ARRAY || type.id() == LogicalTypeId::LIST) {
    const auto& items = type.id() == LogicalTypeId::ARRAY
                            ? duckdb::ArrayValue::GetChildren(value)
                            : duckdb::ListValue::GetChildren(value);
    std::string array = "[";
    for (const auto& item : items) {
      if (array.size() > 1) {
        array += ',';
      }
      array += JsonValue(item);
    }
    return array + "]";
  }
  const auto text = value.ToString();
  if (type.id() == LogicalTypeId::BOOLEAN || type.IsIntegral() ||
      ((type.id() == LogicalTypeId::DOUBLE ||
        type.id() == LogicalTypeId::FLOAT) &&
       std::isfinite(value.GetValue<double>()))) {
    return text;
  }
  return Quote(text);
}

}  // namespace

Table::Table(const fs::path& path, duckdb::DatabaseInstance& database,
             TableSchema schema)
    : path_(path),
      database_(database),
      schema_(std::move(schema)),
      directory_(path) {
  irs::IndexWriterOptions options;
  options.db = &database_;
  options.reader_options.db = &database_;
  memory_writer_ = irs::IndexWriter::Make(memory_, std::move(options));
  auto lock = LockDirectory(path_);
  Open(ReadManifest(path_));
}

void Table::Write(const fs::path& path, duckdb::DatabaseInstance& database,
                  const TableSchema& schema, simdjson::dom::array documents) {
  irs::MMapDirectory dir(path);
  uint64_t last_id = 0;
  WriteManifest(dir, "segments",
                WriteSegments(dir, database, schema, documents, last_id));
}

const Table::SearchSegment& Table::FindSegment(const Snapshot& segments,
                                               std::size_t ordinal) {
  auto it =
      std::ranges::lower_bound(segments, ordinal, {}, &SearchSegment::ordinal);
  if (it == segments.end() || it->ordinal != ordinal)
    throw std::logic_error("primary key references a missing segment");
  return *it;
}

std::vector<uint8_t> Table::PlanColumnUpdates(simdjson::dom::array documents,
                                              const Snapshot& segments,
                                              const KeyMap& keys) const {
  std::vector<uint8_t> modes(documents.size(), 0);
  const auto& primary = schema_.fields[*schema_.primary_key];
  if (!std::ranges::any_of(schema_.fields, [&](const Field& field) {
        return &field != &primary && field.stored && !field.indexed &&
               irs::ColumnPatch::Patchable(field.type);
      }))
    return modes;

  std::map<std::string, simdjson::dom::object> prior_inputs;
  size_t i = 0;
  for (auto doc : documents) {
    auto object = doc.get_object().value();
    auto key = InputValue(object, primary).ToString();
    auto prior = prior_inputs.find(key);
    auto found = keys.find(key);
    if (prior != prior_inputs.end() || found != keys.end()) {
      const irs::SubReader* reader = nullptr;
      if (prior == prior_inputs.end()) {
        reader = &FindSegment(segments, found->second.first).reader;
      }
      modes[i] = std::ranges::all_of(schema_.fields, [&](const Field& field) {
        if (&field == &primary) return true;
        if (!field.indexed &&
            (!field.stored || irs::ColumnPatch::Patchable(field.type)))
          return true;
        // An indexed-only field can be compared when its previous input is in
        // this batch.
        if (reader && !field.stored) return false;
        auto old = reader ? ReadColumn(*reader, found->second.second, field.id,
                                       field.type)
                          : InputValue(prior->second, field);
        return SameValue(old, InputValue(object, field));
      });
    }
    prior_inputs.insert_or_assign(std::move(key), object);
    ++i;
  }
  return modes;
}

void Table::ApplySegments(Snapshot& segments, std::size_t offset,
                          const irs::DirectoryReader& added,
                          const KeyMap& existing, KeyMap& changes) const {
  const auto first_ordinal = offset;
  for (const auto& reader : added.GetReaders()) {
    segments.push_back({offset++, reader, reader.docs_count(), nullptr});
  }
  if (!schema_.primary_key) return;
  HiddenRows hidden;
  // Group by target segment and row. For repeated column writes the last one
  // wins.
  std::map<size_t, std::map<irs::doc_id_t, RowId>> patches;
  const auto& primary = schema_.fields[*schema_.primary_key];
  for (size_t i = 0; i < added.size(); ++i) {
    const auto& segment = added[i];
    auto* columns = segment.GetColReader();
    auto* column = columns ? columns->Column(primary.id) : nullptr;
    if (!column)
      throw std::runtime_error("missing primary key column: " + primary.name);
    auto* marker = columns->Column(kColumnUpdateMarker);
    irs::ReadContext context{*columns};
    auto scan = column->InitScan(context);
    duckdb::Vector key_value(primary.type, 1);
    for (irs::doc_id_t n = 0; n < segment.docs_count(); ++n) {
      column->Scan(scan, key_value, 1);
      auto value = key_value.GetValue(0);
      if (value.IsNull())
        throw std::runtime_error("null primary key: " + primary.name);
      auto key = value.ToString();
      RowId id{first_ordinal + i, n + irs::doc_limits::min()};
      auto lookup = [&](const KeyMap& map) -> const RowId* {
        auto it = map.find(key);
        return it == map.end() ? nullptr : &it->second;
      };
      auto* old = lookup(changes);
      if (!old) old = lookup(existing);
      if (marker && ReadColumn(segment, id.second, kColumnUpdateMarker,
                               duckdb::LogicalType::BOOLEAN)
                        .GetValue<bool>()) {
        if (!old)
          throw std::runtime_error("column update has no base row: " + key);
        patches[old->first][old->second] = id;
        hidden.insert(id);
      } else {
        if (old) hidden.insert(*old);
        changes.insert_or_assign(std::move(key), id);
      }
    }
  }
  for (const auto& [ordinal, rows] : patches) {
    auto target = std::ranges::lower_bound(segments, ordinal, {},
                                           &SearchSegment::ordinal);
    if (target == segments.end() || target->ordinal != ordinal)
      throw std::logic_error("missing column update target");
    ApplyColumnPatches(*target, rows, added, first_ordinal, hidden);
  }
  // Row IDs are sorted by manifest ordinal. Only changed segments need a new
  // mask.
  auto row = hidden.begin();
  for (auto& segment : segments) {
    auto first = row;
    while (row != hidden.end() && row->first == segment.ordinal) ++row;
    if (first == row) continue;
    segment.live_docs -= std::distance(first, row);
    if (segment.live_docs == 0) continue;
    auto mask =
        segment.hidden
            ? std::make_shared<std::vector<bool>>(*segment.hidden)
            : std::make_shared<std::vector<bool>>(segment.reader.docs_count());
    for (; first != row; ++first) {
      (*mask)[first->second - irs::doc_limits::min()] = true;
    }
    segment.hidden = std::move(mask);
  }
  std::erase_if(segments, [](const SearchSegment& segment) {
    return segment.live_docs == 0;
  });
}

void Table::ApplyColumnPatches(SearchSegment& target,
                               const std::map<irs::doc_id_t, RowId>& rows,
                               const irs::DirectoryReader& source,
                               std::size_t first_ordinal,
                               const HiddenRows& hidden) const {
  auto impl = target.reader.GetImpl();
  std::vector<irs::FieldPatch> fields(impl->Patches().begin(),
                                      impl->Patches().end());
  bool changed = false;
  for (const auto& field : schema_.fields) {
    if (&field == &schema_.fields[*schema_.primary_key] || !field.stored ||
        field.indexed || !irs::ColumnPatch::Patchable(field.type))
      continue;
    std::vector<uint64_t> offsets;
    duckdb::Vector values(field.type, rows.size());
    for (const auto& [id, src] : rows) {
      if (hidden.contains({target.ordinal, id})) continue;
      auto value = ReadColumn(source[src.first - first_ordinal], src.second,
                              field.id, field.type);
      if (SameValue(value, ReadColumn(target.reader, id, field.id, field.type)))
        continue;
      values.SetValue(offsets.size(), value);
      offsets.push_back(id - irs::doc_limits::min());
    }
    if (offsets.empty()) continue;
    duckdb::FlatVector::SetSize(values, offsets.size());
    auto entry = std::ranges::find(fields, field.id, &irs::FieldPatch::field);
    if (entry == fields.end()) {
      fields.push_back({field.id, {}});
      entry = std::prev(fields.end());
    }
    changed = true;
    entry->patch = irs::ColumnPatch::Merge(entry->patch.get(), field.type,
                                           offsets, values);
  }
  if (changed) target.reader = irs::SegmentReader(impl->WithPatches(fields));
}

void Table::Publish(std::shared_ptr<const Snapshot> snapshot) {
  std::lock_guard lock(snapshot_mutex_);
  snapshot_.swap(snapshot);
}

irs::SegmentReader Table::IndexInMemory(simdjson::dom::array documents,
                                        std::span<const uint8_t> modes) {
  auto batch = memory_writer_->GetBatch();
  auto id = memory_writer_->NextSegmentId();
  batch.SetHot(id);
  const bool mark_rows = std::ranges::contains(modes, 1);
  size_t i = 0;
  for (auto doc : documents) {
    InsertRow(batch, schema_, doc.get_object().value(),
              mark_rows ? std::optional<bool>(modes[i]) : std::nullopt);
    ++i;
  }
  return irs::SegmentReader(batch.SealPiece(id));
}

void Table::Open(std::string manifest) {
  irs::IndexReaderOptions options;
  options.db = &database_;
  auto disk = irs::OpenSegmentList(directory_, manifest, options);
  uint64_t last_id = 0;
  for (const auto& segment : disk) {
    uint64_t id;
    if (!irs::ParseSegmentId(segment.Meta().name, id)) {
      throw std::runtime_error("invalid segment ID");
    }
    last_id = std::max(last_id, id);
  }
  UpdateLog log(path_, last_id);
  KeyMap keys;
  auto snapshot = std::make_shared<Snapshot>();
  ApplySegments(*snapshot, 0, disk, {}, keys);
  size_t count = disk.size();
  std::vector<uint8_t> buffered;
  log.Replay([&](simdjson::dom::array docs) {
    auto modes = PlanColumnUpdates(docs, *snapshot, keys);
    irs::DirectoryReader added(memory_, options, {IndexInMemory(docs, modes)},
                               false);
    KeyMap changed;
    ApplySegments(*snapshot, count++, added, keys, changed);
    for (auto& [key, id] : changed) keys.insert_or_assign(key, id);
    buffered.insert(buffered.end(), modes.begin(), modes.end());
  });
  log_size_ = log.Size();
  buffered_column_updates_.swap(buffered);
  last_segment_id_ = last_id;
  segment_count_ = count;
  manifest_ = std::move(manifest);
  std::unique_lock key_lock(keys_mutex_);
  keys_.swap(keys);
  Publish(std::move(snapshot));
}

void Table::Upsert(simdjson::dom::array documents) {
  if (!schema_.primary_key)
    throw std::invalid_argument("table has no primary_key: " + schema_.name);
  if (documents.size() == 0) return;
  std::lock_guard lock(writer_mutex_);
  auto file_lock = LockDirectory(path_);
  auto current = ReadManifest(path_);
  if (current != manifest_ ||
      UpdateLog(path_, last_segment_id_).Size() != log_size_) {
    Open(std::move(current));
  }

  UpdateLog log(path_, last_segment_id_);
  auto modes = PlanColumnUpdates(documents, *snapshot_, keys_);
  const bool flush =
      buffered_column_updates_.size() + documents.size() >= schema_.flush_docs;
  auto last_id = last_segment_id_;
  irs::TrackingDirectory pending(directory_);
  absl::Cleanup rollback = [&] { pending.RemoveTracked(); };
  irs::IndexReaderOptions options;
  options.db = &database_;
  std::string manifest;
  irs::DirectoryReader added;
  if (flush) {
    auto segments =
        WriteSegments(pending, database_, schema_, documents, last_id,
                      buffered_column_updates_.empty() ? nullptr : &log,
                      buffered_column_updates_, modes);
    added = irs::OpenSegmentList(directory_, segments, options);
    manifest = manifest_ + segments;
  } else {
    added = irs::DirectoryReader(memory_, options,
                                 {IndexInMemory(documents, modes)}, false);
  }
  KeyMap keys;
  auto snapshot = std::make_shared<Snapshot>(*snapshot_);
  ApplySegments(*snapshot, segment_count_, added, keys_, keys);
  if (flush) {
    WriteManifest(pending, "segments.pending", manifest);
    SyncPath(path_ / "segments.pending");
    fs::rename(path_ / "segments.pending", path_ / "segments");
    std::move(rollback).Cancel();
    manifest_.swap(manifest);
    last_segment_id_ = last_id;
    buffered_column_updates_.clear();
    log_size_ = 0;
  } else {
    auto record = simdjson::minify(documents) + "\n";
    const auto original_size = buffered_column_updates_.size();
    buffered_column_updates_.insert(buffered_column_updates_.end(),
                                    modes.begin(), modes.end());
    absl::Cleanup undo_modes = [&] {
      buffered_column_updates_.resize(original_size);
    };
    log.Append(record, log_size_);
    std::move(undo_modes).Cancel();
    log_size_ += record.size();
  }
  segment_count_ += added.size();
  {
    std::unique_lock key_lock(keys_mutex_);
    keys_.merge(keys);
    for (const auto& [key, id] : keys) keys_.at(key) = id;
    Publish(std::move(snapshot));
  }
  if (flush) {
    SyncPath(path_);
    log.Retire();
  }
}

std::string Table::ReadRow(const irs::SubReader& segment,
                           irs::doc_id_t id) const {
  std::string row = "{";
  for (const auto& field : schema_.fields) {
    if (!field.stored) {
      continue;
    }
    if (row.size() > 1) {
      row += ',';
    }
    row += Quote(field.name) + ":" +
           JsonValue(ReadColumn(segment, id, field.id, field.type));
  }
  return row + "}";
}

std::string Table::SearchByPrimaryKey(std::string_view value) const {
  irs::SegmentReader reader;
  irs::doc_id_t doc;
  {
    std::shared_lock lock(keys_mutex_);
    auto key = keys_.find(value);
    if (key == keys_.end()) return R"({"ok":true,"hits":[]})";
    const auto [ordinal, id] = key->second;
    reader = FindSegment(*snapshot_, ordinal).reader;
    doc = id;
  }
  return R"({"ok":true,"hits":[)" + ReadRow(reader, doc) + "]}";
}

std::string Table::Search(std::string_view field,
                          std::string_view value) const {
  auto leaf = schema_.leaves.find(field);
  if (leaf == schema_.leaves.end()) {
    throw std::invalid_argument("unknown indexed field: " + std::string(field));
  }
  if (schema_.primary_key) {
    const auto& key = schema_.fields[*schema_.primary_key];
    if (key.name == field && !key.tokenizer) return SearchByPrimaryKey(value);
  }
  irs::ByTerm filter;
  *filter.mutable_field_id() = leaf->second;
  *filter.mutable_dict() = irs::StringLayout::kStrings;
  filter.mutable_options()->term = irs::ViewCast<irs::byte_type>(value);
  std::string hits;
  std::shared_ptr<const Snapshot> snapshot;
  {
    std::lock_guard lock(snapshot_mutex_);
    snapshot = snapshot_;
  }
  for (const auto& entry : *snapshot) {
    const auto& segment = entry.reader;
    auto query = filter.PrepareSegment(segment, {});
    auto plan = query ? query->PlanLead({}) : nullptr;
    if (!plan) {
      continue;
    }
    for (auto id = plan->Next(); !irs::doc_limits::eof(id); id = plan->Next()) {
      if (entry.hidden && (*entry.hidden)[id - irs::doc_limits::min()])
        continue;
      if (!hits.empty()) {
        hits += ',';
      }
      hits += ReadRow(segment, id);
    }
  }
  return R"({"ok":true,"hits":[)" + hits + "]}";
}

}  // namespace web3
