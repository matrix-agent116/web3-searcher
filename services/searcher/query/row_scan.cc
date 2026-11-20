#include "searcher/query/row_scan.h"

#include <algorithm>
#include <iresearch/index/document_mask.hpp>
#include <iresearch/search/queries/docs_mask_query.hpp>
#include <numeric>

#include "common/http.h"
#include "searcher/table/lucene_query.h"
#include "searcher/table/row_codec.h"
#include "searcher/table/snapshot.h"

namespace web3::searcher {

namespace {

// A column-only update can extend the physical document range beyond the last
// row of a stored column. These update records are masked in the snapshot.
irs::doc_id_t ScanEnd(const irs::SegmentReader& segment) {
  const irs::doc_id_t end = irs::doc_limits::min() + segment.docs_count();
  const auto* mask = segment.docs_mask();
  if (!mask || mask->Max() != end - 1) return end;
  auto first = static_cast<irs::doc_id_t>(irs::doc_limits::min() +
                                          segment.live_docs_count());
  auto last = end - 1;
  while (first < last) {
    const auto mid = std::midpoint(first, last);
    if (mask->CountInRange(mid, end) == end - mid)
      last = mid;
    else
      first = mid + 1;
  }
  return first;
}

}  // namespace

size_t ScanColumns::Add(const Field& field) {
  auto pos = std::ranges::find(projections, field.id,
                               &irs::ColumnstoreProjection::column_id);
  if (pos != projections.end()) return pos->output_slot;
  auto slot = types.size();
  projections.push_back(
      {.output_slot = slot, .column_id = field.id, .extract_path = {}});
  types.push_back(field.type);
  return slot;
}

RowScan::RowScan(const Snapshot& snapshot, const irs::Filter& filter,
                 const ScanColumns& columns, uint64_t limit)
    : snapshot_(snapshot),
      filter_(filter),
      batcher_(columns.projections, irs::field_limits::invalid(), false),
      remaining_(limit) {}

bool RowScan::OpenSegment(duckdb::ClientContext* context) {
  while (segment_ < snapshot_.Segments().size()) {
    docs_.reset();
    sparse_.reset();
    if (context) context->InterruptCheck();
    auto index = segment_++;
    const auto& segment = snapshot_.Segments()[index].reader;
    query_ = irs::PrepareMasked(filter_, segment, {});
    if (!query_ || irs::QueryBuilder::IsEmpty(*query_)) continue;
    end_ = ScanEnd(segment);
    // Rare terms can skip entire posting gaps; visiting every vector-sized
    // window of a large segment would penalize transaction-hash lookups.
    if (query_->EstimateMax() <=
        std::max<uint64_t>(1, segment.docs_count() / STANDARD_VECTOR_SIZE)) {
      sparse_ = query_->PlanLead({});
      if (!sparse_) continue;
      next_ = sparse_->Next();
    } else {
      docs_ = query_->PlanDocs({.span = STANDARD_VECTOR_SIZE});
      if (!docs_) continue;
      next_ = irs::doc_limits::min();
    }
    if (next_ >= end_) continue;
    batcher_.BeginSegment(index, segment.GetColReader(), context);
    return true;
  }
  return false;
}

bool RowScan::Next(duckdb::DataChunk& batch, duckdb::ClientContext* context) {
  for (;;) {
    if (context) context->InterruptCheck();
    if (next_ >= end_ || remaining_ == 0) {
      batcher_.Finalize();
      if (!batcher_.Ready() && (!remaining_ || !OpenSegment(context))) {
        next_ = end_;
        batch.SetChildCardinality(0);
        return false;
      }
    }
    if (batcher_.Ready()) {
      batch.SetChildCardinality(batcher_.Emit(batch).count);
      return true;
    }
    const auto window = batcher_.OpenWindow(next_ - irs::doc_limits::min());
    if (!window) {
      if (!batcher_.Ready())
        throw std::runtime_error(
            "empty IResearch column window for a live row");
      continue;
    }
    if (sparse_) {
      *batcher_.WindowHead() = next_;
      batcher_.CommitWindow(1);
      --remaining_;
      next_ = remaining_ ? sparse_->Next() : end_;
    } else {
      auto width = std::min<uint64_t>({window, remaining_, end_ - next_});
      const auto stop = static_cast<irs::doc_id_t>(next_ + width);
      auto hits = docs_->Run(next_, stop, batcher_.WindowHead());
      batcher_.CommitWindow(hits);
      remaining_ -= hits;
      next_ = stop;
    }
  }
}

void RowProjection::Add(const Field& field, std::string_view alias) {
  outputs_.push_back({columns_.Add(field),
                      (outputs_.empty() ? "" : ",") + Quote(alias) + ":"});
}

std::string RowProjection::Search(const Snapshot& snapshot,
                                  const TableSchema& schema,
                                  std::string_view expression,
                                  SearchLimit limit) const {
  auto filter = ParseLuceneQuery(schema, expression);
  RowScan scan(snapshot, *filter, columns_, limit.value());
  duckdb::DataChunk batch;
  if (!columns_.types.empty())
    batch.Initialize(duckdb::Allocator::DefaultAllocator(), columns_.types);
  std::string body = R"({"ok":true,"hits":[)";
  bool first = true;
  while (scan.Next(batch)) {
    for (size_t row = 0; row < batch.size(); ++row) {
      body += first ? "{" : ",{";
      first = false;
      for (const auto& out : outputs_) {
        auto value = JsonValue(batch.GetValue(out.slot, row));
        if (body.size() + out.key.size() + value.size() + 3 > kMaxHttpBodyBytes)
          throw std::length_error("search response exceeds 16 MiB");
        body += out.key;
        body += value;
      }
      body += '}';
    }
    batch.Reset();
  }
  return body + "]}";
}

}  // namespace web3::searcher
