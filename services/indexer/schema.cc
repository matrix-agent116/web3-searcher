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
#include "indexer/schema.h"

#include <algorithm>
#include <duckdb/common/vector/flat_vector.hpp>
#include <iresearch/analysis/text_tokenizer.hpp>
#include <iresearch/formats/column/column_writer.hpp>
#include <set>
#include <stdexcept>

namespace web3 {
namespace {
using duckdb::LogicalType;
using duckdb::LogicalTypeId;
using duckdb::Value;
void VisitLeaves(const LogicalType& type, const std::string& path,
                 const Value* value, auto&& visit) {
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

TableSchema ReadTable(Json json, duckdb::ClientContext& context) {
  TableSchema schema;
  schema.name = std::string{json["name"].get_string().value()};
  auto flush_docs = json["flush_docs"];
  if (flush_docs.error() != simdjson::NO_SUCH_FIELD &&
      (flush_docs.get_uint64().get(schema.flush_docs) ||
       schema.flush_docs == 0)) {
    throw std::invalid_argument("table " + schema.name +
                                ": flush_docs must be a positive integer");
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
                  [&](const auto& leaf, const Value*) {
                    schema.leaves.emplace(leaf, schema.leaves.size() + 1);
                  });
    }
    schema.fields.push_back(std::move(field));
  }
  if (schema.fields.empty()) {
    throw std::invalid_argument("no fields in table " + schema.name);
  }
  auto primary_key = json["primary_key"];
  if (primary_key.error() != simdjson::NO_SUCH_FIELD) {
    auto name = primary_key.get_string().value();
    auto field = std::ranges::find(schema.fields, name, &Field::name);
    if (field == schema.fields.end() || field->type.IsNested()) {
      throw std::invalid_argument("primary_key must name a scalar field in " +
                                  schema.name);
    }
    schema.primary_key =
        static_cast<std::size_t>(field - schema.fields.begin());
  }
  return schema;
}

Value Convert(Json json, const LogicalType& type);

Value Member(simdjson::dom::object object, const std::string& name,
             const LogicalType& type) {
  auto member = object[name];
  return member.error() == simdjson::NO_SUCH_FIELD
             ? Value(type)
             : Convert(member.value(), type);
}

Value Convert(Json json, const LogicalType& type) {
  if (json.is_null()) {
    return Value(type);
  }
  if (type.id() == LogicalTypeId::STRUCT) {
    auto object = json.get_object().value();
    duckdb::vector<Value> members;
    for (const auto& [name, child] : duckdb::StructType::GetChildTypes(type)) {
      members.push_back(Member(object, name.GetIdentifierName(), child));
    }
    return Value::STRUCT(type, std::move(members));
  }
  if (type.id() == LogicalTypeId::LIST || type.id() == LogicalTypeId::ARRAY) {
    const bool fixed = type.id() == LogicalTypeId::ARRAY;
    const auto& child = fixed ? duckdb::ArrayType::GetChildType(type)
                              : duckdb::ListType::GetChildType(type);
    auto array = json.get_array().value();
    if (fixed && array.size() != duckdb::ArrayType::GetSize(type)) {
      throw std::runtime_error("wrong array length for " + type.ToString());
    }
    duckdb::vector<Value> items;
    for (auto item : array) {
      items.push_back(Convert(item, child));
    }
    return fixed ? Value::ARRAY(child, std::move(items))
                 : Value::LIST(child, std::move(items));
  }
  if (json.is_object() || json.is_array()) {
    throw std::runtime_error("expected scalar " + type.ToString());
  }
  auto text = json.is_string() ? std::string{json.get_string().value()}
                               : simdjson::minify(json);
  return Value(std::move(text)).DefaultCastAs(type, true);
}

}  // namespace

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

duckdb::Value InputValue(simdjson::dom::object object, const Field& field) {
  return Member(object, field.name, field.type);
}

void InsertRow(irs::IndexWriter::Transaction& batch, const TableSchema& schema,
               simdjson::dom::object object,
               std::optional<bool> column_update) {
  auto doc = batch.Insert();
  auto append = [&](irs::field_id id, const Value& value) {
    auto& column = doc.GetColWriter()->OpenColumn(id, value.type());
    duckdb::Vector row{value.type(), 1};
    row.SetValue(0, value);
    duckdb::FlatVector::SetSize(row, 1);
    column.Append(doc.DocId() - irs::doc_limits::min(), row, 1);
  };
  // Mark every row in mixed segments, including trailing full replacements.
  if (column_update)
    append(kColumnUpdateMarker, Value::BOOLEAN(*column_update));
  const bool columns_only = column_update.value_or(false);
  for (const auto& field : schema.fields) {
    auto value = InputValue(object, field);
    bool is_key =
        schema.primary_key && &field == &schema.fields[*schema.primary_key];
    if (is_key && value.IsNull()) {
      throw std::invalid_argument("missing or null primary key: " + field.name);
    }
    if (is_key || (field.stored && (!columns_only || !field.indexed))) {
      append(field.id, value);
    }
    if (field.indexed && !columns_only) {
      VisitLeaves(
          field.type, field.name, &value,
          [&](const auto& path, const Value* leaf) {
            const auto term = leaf->ToString();
            if (field.tokenizer) {
              if (!doc.WithTokens(
                      schema.leaves.at(path), irs::StringLayout::kStrings,
                      irs::IndexFeatures::Freq, nullptr,
                      [&](irs::FieldInverter& inverter, irs::TokenSink& sink) {
                        inverter.Configure(field.tokenizer->Traits());
                        if (!field.tokenizer->Fill(duckdb::string_t{term},
                                                   doc.DocId(), sink,
                                                   {inverter.Layout()})) {
                          throw std::runtime_error("could not tokenize " +
                                                   path);
                        }
                      })) {
                throw std::runtime_error("could not index " + path);
              }
              return;
            }
            if (!doc.WithField(
                    schema.leaves.at(path), irs::StringLayout::kStrings,
                    irs::IndexFeatures::None,
                    [&](irs::FieldInverter& inverter) {
                      return inverter.InvertKeywords([&](auto&& emit) {
                        emit(duckdb::string_t{term}, doc.DocId());
                      });
                    })) {
              throw std::runtime_error("could not index " + path);
            }
          });
    }
  }
}

}  // namespace web3
