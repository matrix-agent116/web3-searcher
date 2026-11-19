#ifndef WEB3_SEARCHER_SERVICES_INDEXER_SCHEMA_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_SCHEMA_H_

#include <cstdint>
#include <duckdb.hpp>
#include <filesystem>
#include <iresearch/analysis/tokenizer.hpp>
#include <iresearch/index/index_writer.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/http.h"

namespace web3 {

struct Field {
  std::string name;
  duckdb::LogicalType type;
  bool stored;
  bool indexed;
  irs::field_id id;
  irs::analysis::Tokenizer::ptr tokenizer;
};

struct TableSchema {
  std::string name;
  // Optional seed data for tests. Production schemas omit this field.
  std::optional<std::filesystem::path> documents;
  std::vector<Field> fields;
  std::optional<std::size_t> primary_key;
  // Count every input document, including repeated updates to the same key.
  std::uint64_t flush_docs = 256;
  std::map<std::string, irs::field_id, std::less<>> leaves;
};

std::vector<TableSchema> ReadSchema(const std::filesystem::path& config,
                                    duckdb::ClientContext& context);
// Schema fields start at 1. Column 0 identifies stored-column updates on
// replay.
inline constexpr irs::field_id kColumnUpdateMarker = 0;
duckdb::Value InputValue(simdjson::dom::object object, const Field& field);
void InsertRow(irs::IndexWriter::Transaction& batch, const TableSchema& table,
               simdjson::dom::object object,
               std::optional<bool> column_update = std::nullopt);

}  // namespace web3

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_SCHEMA_H_
