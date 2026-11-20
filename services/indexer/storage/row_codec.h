#ifndef WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_ROW_CODEC_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_ROW_CODEC_H_

#include <simdjson.h>

#include <iresearch/index/index_writer.hpp>
#include <span>
#include <string>

#include "indexer/config/schema.h"

namespace web3::indexer {

// Stored per row so checkpoints can replay operations from different requests.
inline constexpr irs::field_id kUpdateField = 0;
const duckdb::LogicalType& UpdateMetadataType();
duckdb::Value InputValue(simdjson::dom::object object, const Field& field);
void InsertRow(irs::IndexWriter::Transaction& batch, const TableSchema& schema,
               simdjson::dom::object object,
               std::span<const irs::field_id> match_fields = {},
               bool columns_only = false);

duckdb::Value ReadColumn(const irs::SubReader& segment, irs::doc_id_t id,
                         irs::field_id field, const duckdb::LogicalType& type);
std::string ReadRow(const TableSchema& schema, const irs::SubReader& segment,
                    irs::doc_id_t id);

}  // namespace web3::indexer

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_ROW_CODEC_H_
