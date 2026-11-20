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

#include "indexer/table/snapshot.h"

#include <algorithm>
#include <duckdb/common/vector/flat_vector.hpp>
#include <iresearch/formats/column/col_reader.hpp>
#include <iresearch/index/document_mask.hpp>
#include <iresearch/index/segment_reader_impl.hpp>
#include <iresearch/search/filters/boolean_filter.hpp>
#include <iresearch/search/filters/term_filter.hpp>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "indexer/table/memory_segment.h"
#include "indexer/table/row_codec.h"

namespace web3::indexer {
namespace {

using RowId = std::pair<std::size_t, irs::doc_id_t>;

bool SameValue(const duckdb::Value& left, const duckdb::Value& right) {
  // Indexed terms use ToString(), including the spelling of signed zero.
  return duckdb::Value::NotDistinctFrom(left, right) &&
         left.ToString() == right.ToString();
}

const SearchSegment& FindSegment(std::span<const SearchSegment> segments,
                                 std::size_t ordinal) {
  auto it =
      std::ranges::lower_bound(segments, ordinal, {}, &SearchSegment::ordinal);
  if (it == segments.end() || it->ordinal != ordinal)
    throw std::logic_error("matched row references a missing segment");
  return *it;
}

template <typename ReadValue>
std::vector<std::string> MatchKey(const TableSchema& schema,
                                  std::span<const irs::field_id> fields,
                                  ReadValue read) {
  std::vector<std::string> values;
  values.reserve(fields.size());
  for (auto id : fields) {
    const auto& field = schema.fields[id - 1];
    const auto value = read(field);
    if (value.IsNull())
      throw std::invalid_argument("missing or null match value: " + field.name);
    values.push_back(value.ToString());
  }
  return values;
}

std::optional<RowId> FindMatch(const TableSchema& schema,
                               std::span<const SearchSegment> segments,
                               std::span<const irs::field_id> fields,
                               const std::vector<std::string>& values,
                               irs::doc_id_t before = irs::doc_limits::eof(),
                               const std::set<RowId>* removed = nullptr) {
  irs::BooleanFilter filter;
  for (size_t i = 0; i < fields.size(); ++i) {
    filter.Add({.field = schema.ExactTerm(schema.fields[fields[i] - 1]),
                .dict = irs::StringLayout::kStrings,
                .term = irs::bstring{irs::ViewCast<irs::byte_type>(
                    std::string_view(values[i]))}},
               irs::Occur::Must);
  }
  std::optional<RowId> hit;
  for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
    const auto limit = std::exchange(before, irs::doc_limits::eof());
    auto query = filter.PrepareSegment(it->reader, {});
    auto plan = query ? query->PlanLead({}) : nullptr;
    if (!plan) continue;
    const auto* mask = it->reader.docs_mask();
    for (auto id = plan->Next(); id < limit; id = plan->Next()) {
      RowId row{it->ordinal, id};
      if ((mask && mask->Contains(id)) || (removed && removed->contains(row)))
        continue;
      if (hit) throw AmbiguousMatch();
      hit = row;
    }
  }
  return hit;
}

void ApplyColumnPatches(const TableSchema& schema, SearchSegment& target,
                        const std::map<irs::doc_id_t, RowId>& rows,
                        std::span<const irs::SegmentReader> source,
                        std::size_t first_ordinal) {
  auto impl = target.reader.GetImpl();
  std::vector<irs::FieldPatch> fields(impl->Patches().begin(),
                                      impl->Patches().end());
  bool changed = false;
  for (const auto& field : schema.fields) {
    if (!field.stored || field.indexed ||
        !irs::ColumnPatch::Patchable(field.type))
      continue;
    std::vector<uint64_t> offsets;
    duckdb::Vector values(field.type, rows.size());
    for (const auto& [id, src] : rows) {
      if (auto* mask = target.reader.docs_mask(); mask && mask->Contains(id))
        continue;
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

}  // namespace

void Snapshot::CommitBlock(uint64_t block_number) {
  if (block_number <= block_number_)
    throw std::invalid_argument("block already finalized");
  block_number_ = block_number;
}

void Snapshot::AdvanceBlock(absl::int128 block_number) {
  if (block_number < block_number_)
    throw std::invalid_argument(
        "block_number precedes the current table height");
  block_number_ = block_number;
}

std::vector<uint8_t> Snapshot::PlanColumnUpdates(
    const TableSchema& schema, simdjson::dom::array documents,
    std::span<const irs::field_id> match_fields) const {
  std::vector<uint8_t> modes(documents.size(), 0);
  if (!std::ranges::any_of(schema.fields, [&](const Field& field) {
        return field.stored && !field.indexed &&
               irs::ColumnPatch::Patchable(field.type);
      }))
    return modes;

  std::map<std::vector<std::string>, simdjson::dom::object> prior_inputs;
  size_t i = 0;
  for (auto doc : documents) {
    auto object = doc.get_object().value();
    auto key = MatchKey(schema, match_fields, [&](const Field& field) {
      return InputValue(object, field);
    });
    auto prior = prior_inputs.find(key);
    std::optional<RowId> found;
    if (prior == prior_inputs.end()) {
      found = FindMatch(schema, segments_, match_fields, key);
    }
    if (prior != prior_inputs.end() || found) {
      const irs::SubReader* reader = nullptr;
      if (found) {
        reader = &FindSegment(segments_, found->first).reader;
      }
      modes[i] = std::ranges::all_of(schema.fields, [&](const Field& field) {
        if (std::ranges::binary_search(match_fields, field.id)) return true;
        if (!field.indexed &&
            (!field.stored || irs::ColumnPatch::Patchable(field.type)))
          return true;
        // An indexed-only field can be compared when its previous input is in
        // this batch.
        if (reader && !field.stored) return false;
        auto old =
            reader ? ReadColumn(*reader, found->second, field.id, field.type)
                   : InputValue(prior->second, field);
        return SameValue(old, InputValue(object, field));
      });
    }
    prior_inputs.insert_or_assign(std::move(key), object);
    ++i;
  }
  return modes;
}

void Snapshot::ApplySegments(const TableSchema& schema, std::size_t offset,
                             std::span<const irs::SegmentReader> added,
                             bool first_is_compacted) {
  for (size_t i = 0; i < added.size(); ++i) {
    const auto ordinal = offset + i;
    const auto& reader = added[i];
    if (i == 0 && first_is_compacted) {
      // This segment contains materialized live rows. Replaying its historical
      // match fields could incorrectly collapse rows with duplicate values.
      segments_.push_back({ordinal, reader});
      continue;
    }
    auto* columns = reader.GetColReader();
    const auto* selector = columns ? columns->Column(kUpdateField) : nullptr;
    if (!selector || selector->Type() != UpdateMetadataType()) {
      throw std::runtime_error(
          "incompatible table format; rebuild from source documents: " +
          schema.name);
    }
    segments_.push_back({ordinal, reader});
    std::set<RowId> removed;
    std::map<size_t, std::map<irs::doc_id_t, RowId>> patches;
    irs::ReadContext context{*columns};
    auto scan = selector->InitScan(context);
    duckdb::Vector selection(UpdateMetadataType(), 1);
    for (irs::doc_id_t n = 0; n < reader.docs_count(); ++n) {
      selector->Scan(scan, selection, 1);
      const auto metadata = selection.GetValue(0);
      const auto& members = duckdb::StructValue::GetChildren(metadata);
      const bool columns_only = members[0].GetValue<bool>();
      const auto& ids = duckdb::ListValue::GetChildren(members[1]);
      if (ids.empty() && !columns_only) continue;  // Seed document.
      std::vector<irs::field_id> fields;
      for (const auto& item : ids) {
        const auto field = item.GetValue<uint64_t>();
        if (field == 0 || field > schema.fields.size() ||
            (!fields.empty() && field <= fields.back()) ||
            !schema.fields[field - 1].indexed ||
            schema.fields[field - 1].type.IsNested())
          throw std::runtime_error("invalid stored match fields in table: " +
                                   schema.name);
        fields.push_back(field);
      }
      if (fields.empty())
        throw std::runtime_error("column update without match fields");
      const auto id = n + irs::doc_limits::min();
      auto key = MatchKey(schema, fields, [&](const Field& field) {
        return ReadColumn(reader, id, field.id, field.type);
      });
      auto old = FindMatch(schema, segments_, fields, key, id, &removed);
      if (columns_only) {
        if (!old)
          throw std::runtime_error(
              "column update has no matching row in table: " + schema.name);
        patches[old->first][old->second] = {ordinal, id};
        removed.emplace(ordinal, id);
      } else if (old) {
        removed.insert(*old);
      }
    }
    std::map<size_t, irs::DocumentMaskBuilder> removals;
    for (auto [number, id] : removed) removals[number].Add(id);
    for (auto& [number, changes] : removals) {
      auto target = std::ranges::lower_bound(segments_, number, {},
                                             &SearchSegment::ordinal);
      auto impl = target->reader.GetImpl();
      if (auto* old = impl->docs_mask()) changes.Merge(*old);
      auto mask = std::make_shared<const irs::DocumentMask>(
          std::move(changes).Finish());
      target->reader = irs::SegmentReader(impl->WithDocsMask(std::move(mask)));
    }
    for (const auto& [number, rows] : patches) {
      auto target = std::ranges::lower_bound(segments_, number, {},
                                             &SearchSegment::ordinal);
      if (target->reader.live_docs_count())
        ApplyColumnPatches(schema, *target, rows, added, offset);
    }
    std::erase_if(segments_, [](const SearchSegment& entry) {
      return entry.reader.live_docs_count() == 0;
    });
  }
}

void Snapshot::ReplaceMemorySegments(std::span<const MemorySegment> inputs,
                                     const irs::SegmentReader& merged) {
  auto first = std::ranges::lower_bound(segments_, inputs.front().ordinal, {},
                                        &SearchSegment::ordinal);
  auto live = first;
  irs::DocumentMaskBuilder mask;
  std::vector<irs::FieldPatch> patches;
  irs::doc_id_t offset = 0;
  // The raw merge concatenates every physical row, including superseded rows.
  // Translate the latest query view onto those same row offsets.
  for (const auto& input : inputs) {
    const auto docs = input.reader.docs_count();
    if (live == segments_.end() || live->ordinal != input.ordinal) {
      mask.AddRange(offset + irs::doc_limits::min(),
                    offset + irs::doc_limits::min() + docs);
    } else {
      if (const auto* deleted = live->reader.docs_mask())
        mask.AddShifted(*deleted, offset);
      for (const auto& [field, patch] : live->reader.GetImpl()->Patches()) {
        auto it = std::ranges::find(patches, field, &irs::FieldPatch::field);
        if (it == patches.end()) {
          patches.push_back({field, {}});
          it = std::prev(patches.end());
        }
        auto entries = patch->EntriesSince(nullptr);
        for (auto& row : entries.rows) row += offset;
        it->patch = irs::ColumnPatch::Merge(it->patch.get(), patch->Type(),
                                            entries.rows, entries.values);
      }
      ++live;
    }
    offset += docs;
  }
  auto pos = segments_.erase(first, live);
  if (mask.Count() == merged.docs_count()) return;
  auto reader = merged.GetImpl();
  if (!patches.empty()) reader = reader->WithPatches(patches);
  if (!mask.Empty())
    reader = reader->WithDocsMask(
        std::make_shared<const irs::DocumentMask>(std::move(mask).Finish()));
  segments_.insert(
      pos, {inputs.front().ordinal, irs::SegmentReader(std::move(reader))});
}

std::string Snapshot::Search(const TableSchema& schema, std::string_view field,
                             std::string_view value) const {
  auto leaf = schema.leaves.find(field);
  if (leaf == schema.leaves.end()) {
    throw std::invalid_argument("unknown indexed field: " + std::string(field));
  }
  const auto term = irs::ViewCast<irs::byte_type>(value);
  std::string hits;
  irs::ByTerm filter;
  *filter.mutable_field_id() = leaf->second;
  *filter.mutable_dict() = irs::StringLayout::kStrings;
  filter.mutable_options()->term = term;
  for (const auto& entry : Segments()) {
    const auto& segment = entry.reader;
    auto query = filter.PrepareSegment(segment, {});
    auto plan = query ? query->PlanLead({}) : nullptr;
    if (!plan) {
      continue;
    }
    for (auto id = plan->Next(); !irs::doc_limits::eof(id); id = plan->Next()) {
      if (auto* mask = segment.docs_mask(); mask && mask->Contains(id))
        continue;
      if (!hits.empty()) {
        hits += ',';
      }
      hits += ReadRow(schema, segment, id);
    }
  }
  return R"({"ok":true,"hits":[)" + hits + "]}";
}

}  // namespace web3::indexer
