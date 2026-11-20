#ifndef WEB3_SEARCHER_SERVICES_INDEXER_CONFIG_SCHEMA_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_CONFIG_SCHEMA_H_

#include <absl/functional/function_ref.h>
#include <simdjson.h>

#include <cstdint>
#include <duckdb.hpp>
#include <filesystem>
#include <iresearch/analysis/tokenizer.hpp>
#include <iresearch/types.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace web3::indexer {

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
  // Count every input document, including repeated updates to the same key.
  std::uint64_t flush_docs = 256;
  std::uint64_t merge_segments = 16;
  std::map<std::string, irs::field_id, std::less<>> leaves;
  std::vector<irs::field_id> MatchFields(simdjson::dom::array names) const;
  irs::field_id ExactTerm(const Field& field) const;
};

// A null value walks the schema paths; a supplied value visits its non-null
// leaves.
void VisitLeaves(
    const duckdb::LogicalType& type, const std::string& path,
    const duckdb::Value* value,
    absl::FunctionRef<void(const std::string&, const duckdb::Value*)> visit);

std::vector<TableSchema> ReadSchema(const std::filesystem::path& config,
                                    duckdb::ClientContext& context);

}  // namespace web3::indexer

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_CONFIG_SCHEMA_H_
