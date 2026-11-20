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

// Adapted for the Web3 Searcher catalog and JSON query API.
#include "searcher/config/schema.h"

#include <simdjson.h>

#include <algorithm>
#include <iresearch/analysis/text_tokenizer.hpp>
#include <set>
#include <stdexcept>

namespace web3::searcher {
namespace {
using duckdb::LogicalTypeId;

TableSchema ReadTable(simdjson::dom::element json,
                      duckdb::ClientContext& context) {
  TableSchema schema;
  schema.name = std::string{json["name"].get_string().value()};
  auto flush_docs = json["flush_docs"];
  if (flush_docs.error() != simdjson::NO_SUCH_FIELD &&
      (flush_docs.get_uint64().get(schema.flush_docs) ||
       schema.flush_docs == 0)) {
    throw std::invalid_argument("table " + schema.name +
                                ": flush_docs must be a positive integer");
  }
  auto merge_segments = json["merge_segments"];
  if (merge_segments.error() != simdjson::NO_SUCH_FIELD &&
      (merge_segments.get_uint64().get(schema.merge_segments) ||
       schema.merge_segments < 2)) {
    throw std::invalid_argument("table " + schema.name +
                                ": merge_segments must be an integer >= 2");
  }
  auto documents = json["documents"];
  if (documents.error() != simdjson::NO_SUCH_FIELD) {
    auto path = documents.get_string().value();
    if (path.empty() || path.contains('\0')) {
      throw std::invalid_argument("invalid documents path");
    }
    schema.documents = std::filesystem::path(path);
  }
  if (schema.name.contains('\0') || schema.name == "schema.json" ||
      schema.name.empty() || schema.name == "." || schema.name == ".." ||
      schema.name.find_first_of("/\\\\") != std::string::npos) {
    throw std::runtime_error("invalid table name: " + schema.name);
  }
  std::set<std::string> names;
  for (auto entry : json["fields"].get_array().value()) {
    std::string name{entry["name"].get_string().value()};
    if (name.empty() || name.contains('.') || !names.insert(name).second) {
      throw std::runtime_error("invalid or duplicate field: " + name);
    }
    auto type = context.ParseLogicalType(
        std::string{entry["type"].get_string().value()});
    Field field{name,
                std::move(type),
                entry["stored"].get_bool().value(),
                entry["indexed"].get_bool().value(),
                schema.fields.size() + 1,
                {}};
    auto analyzer = entry["analyzer"];
    if (analyzer.error() != simdjson::NO_SUCH_FIELD) {
      if (!field.indexed || field.type.id() != LogicalTypeId::VARCHAR ||
          analyzer.get_string().value() != "text") {
        throw std::runtime_error("text analyzer requires an indexed VARCHAR: " +
                                 name);
      }
      field.tokenizer = irs::analysis::TextTokenizer::Make({});
    }
    if (field.indexed) {
      VisitLeaves(field.type, name, nullptr,
                  [&](const auto& leaf, const duckdb::Value*) {
                    schema.leaves.emplace(leaf, schema.leaves.size() + 1);
                  });
    }
    schema.fields.push_back(std::move(field));
  }
  if (schema.fields.empty()) {
    throw std::invalid_argument("no fields in table " + schema.name);
  }
  return schema;
}

}  // namespace

void VisitLeaves(
    const duckdb::LogicalType& type, const std::string& path,
    const duckdb::Value* value,
    absl::FunctionRef<void(const std::string&, const duckdb::Value*)> visit) {
  if (value && value->IsNull()) {
    return;
  }
  if (type.id() == LogicalTypeId::STRUCT) {
    size_t i = 0;
    for (const auto& [key, child] : duckdb::StructType::GetChildTypes(type)) {
      const auto& name = key.GetIdentifierName();
      if (name.contains('.')) {
        throw std::runtime_error("dot in struct member: " + name);
      }
      VisitLeaves(
          child, path + "." + name,
          value ? &duckdb::StructValue::GetChildren(*value)[i] : nullptr,
          visit);
      ++i;
    }
  } else if (type.id() == LogicalTypeId::LIST ||
             type.id() == LogicalTypeId::ARRAY) {
    const bool fixed = type.id() == LogicalTypeId::ARRAY;
    const auto& child = fixed ? duckdb::ArrayType::GetChildType(type)
                              : duckdb::ListType::GetChildType(type);
    if (value) {
      const auto& items = fixed ? duckdb::ArrayValue::GetChildren(*value)
                                : duckdb::ListValue::GetChildren(*value);
      for (const auto& item : items) {
        VisitLeaves(child, path, &item, visit);
      }
    } else {
      VisitLeaves(child, path, nullptr, visit);
    }
  } else {
    visit(path, value);
  }
}

std::vector<TableSchema> ReadSchema(const std::filesystem::path& path,
                                    duckdb::ClientContext& context) {
  simdjson::dom::parser parser;
  auto root = parser.load(path.string()).value();
  std::vector<TableSchema> schemas;
  std::set<std::string> names;
  for (auto table : root["tables"].get_array().value()) {
    auto schema = ReadTable(table, context);
    if (!names.insert(schema.name).second) {
      throw std::runtime_error("duplicate table: " + schema.name);
    }
    schemas.push_back(std::move(schema));
  }
  if (schemas.empty()) {
    throw std::runtime_error("schema has no tables");
  }
  return schemas;
}

const Field& TableSchema::StoredField(std::string_view field_name) const {
  auto field = std::ranges::find(fields, field_name, &Field::name);
  if (field == fields.end() || !field->stored) {
    throw std::invalid_argument("unknown stored field: " +
                                std::string(field_name));
  }
  return *field;
}

std::vector<irs::field_id> TableSchema::MatchFields(
    simdjson::dom::array names) const {
  if (names.size() == 0)
    throw std::invalid_argument(
        "match_field must be a nonempty array of field names");
  std::vector<irs::field_id> ids;
  ids.reserve(names.size());
  for (auto entry : names) {
    const auto name = entry.get_string().value();
    auto it = std::ranges::find(fields, name, &Field::name);
    if (it == fields.end() || !it->indexed || it->type.IsNested())
      throw std::invalid_argument(
          "match_field must name indexed scalar fields: " + std::string(name));
    ids.push_back(it->id);
  }
  std::ranges::sort(ids);
  if (std::ranges::adjacent_find(ids) != ids.end())
    throw std::invalid_argument("duplicate field in match_field");
  return ids;
}

irs::field_id TableSchema::ExactTerm(const Field& field) const {
  // Text fields keep their public tokens and an exact term for whole-value
  // updates.
  return field.tokenizer ? leaves.size() + field.id : leaves.at(field.name);
}

}  // namespace web3::searcher
