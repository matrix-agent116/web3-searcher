#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_ROW_CODEC_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_ROW_CODEC_H_

#include <simdjson.h>

#include <iresearch/index/index_writer.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "searcher/config/schema.h"

namespace web3::searcher {

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
std::vector<const Field*> StoredFields(
    const TableSchema& schema, std::span<const std::string_view> names = {});
std::string ReadRow(std::span<const Field* const> fields,
                    const irs::SubReader& segment, irs::doc_id_t id);

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_ROW_CODEC_H_
