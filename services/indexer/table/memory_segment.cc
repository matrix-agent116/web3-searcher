#include "indexer/table/memory_segment.h"

#include <stdexcept>

#include "indexer/table/row_codec.h"

namespace web3::indexer {

MemorySegment MemorySegment::Build(size_t ordinal, irs::IndexWriter& writer,
                                   const TableSchema& schema,
                                   const Batch& update) {
  if (update.documents.size() > irs::doc_limits::kMaxDocs)
    throw std::invalid_argument(
        "batch exceeds the IResearch segment document limit");
  if (update.column_updates.size() != update.documents.size())
    throw std::invalid_argument("expected one update mode per document");
  auto batch = writer.GetBatch();
  const auto id = writer.NextSegmentId();
  batch.SetHot(id);
  size_t i = 0;
  for (auto doc : update.documents)
    InsertRow(batch, schema, doc.get_object().value(), update.match_fields,
              update.column_updates[i++]);
  return {ordinal, irs::SegmentReader(batch.SealPiece(id))};
}

}  // namespace web3::indexer
