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

#include "searcher/table/row_codec.h"

#include <algorithm>
#include <cmath>
#include <duckdb/common/vector/flat_vector.hpp>
#include <iresearch/formats/column/col_reader.hpp>
#include <iresearch/formats/column/column_writer.hpp>
#include <stdexcept>
#include <utility>

#include "common/http.h"

namespace web3::searcher {
namespace {
using duckdb::LogicalType;
using duckdb::LogicalTypeId;
using duckdb::Value;

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

std::vector<const Field*> StoredFields(
    const TableSchema& schema, std::span<const std::string_view> names) {
  std::vector<const Field*> fields;
  if (names.empty()) {
    for (const auto& field : schema.fields) {
      if (field.stored) fields.push_back(&field);
    }
    return fields;
  }
  for (auto name : names) {
    const auto& field = schema.StoredField(name);
    if (std::ranges::find(fields, &field) != fields.end())
      throw std::invalid_argument("duplicate selected field: " +
                                  std::string(name));
    fields.push_back(&field);
  }
  return fields;
}

duckdb::Value InputValue(simdjson::dom::object object, const Field& field) {
  return Member(object, field.name, field.type);
}

const duckdb::LogicalType& UpdateMetadataType() {
  static const auto type = LogicalType::STRUCT(
      {{"columns_only", LogicalType::BOOLEAN},
       {"match_fields", LogicalType::LIST(LogicalType::UBIGINT)}});
  return type;
}

void InsertRow(irs::IndexWriter::Transaction& batch, const TableSchema& schema,
               simdjson::dom::object object,
               std::span<const irs::field_id> match_fields, bool columns_only) {
  auto doc = batch.Insert();
  auto append = [&](irs::field_id id, const Value& value) {
    auto& column = doc.GetColWriter()->OpenColumn(id, value.type());
    duckdb::Vector row{value.type(), 1};
    row.SetValue(0, value);
    duckdb::FlatVector::SetSize(row, 1);
    column.Append(doc.DocId() - irs::doc_limits::min(), row, 1);
  };
  auto index_keyword = [&](irs::field_id id, const std::string& term) {
    if (!doc.WithField(id, irs::StringLayout::kStrings,
                       irs::IndexFeatures::None,
                       [&](irs::FieldInverter& inverter) {
                         return inverter.InvertKeywords([&](auto&& emit) {
                           emit(duckdb::string_t{term}, doc.DocId());
                         });
                       })) {
      throw std::runtime_error("could not index keyword in table " +
                               schema.name);
    }
  };
  duckdb::vector<Value> ids;
  for (auto id : match_fields) ids.push_back(Value::UBIGINT(id));
  append(kUpdateField,
         Value::STRUCT(UpdateMetadataType(),
                       {Value::BOOLEAN(columns_only),
                        Value::LIST(LogicalType::UBIGINT, std::move(ids))}));
  for (const auto& field : schema.fields) {
    auto value = InputValue(object, field);
    const bool match = std::ranges::binary_search(match_fields, field.id);
    if (match && value.IsNull())
      throw std::invalid_argument("missing or null match value: " + field.name);
    if (match || (field.stored && (!columns_only || !field.indexed))) {
      append(field.id, value);
    }
    if (field.tokenizer && !columns_only && !value.IsNull()) {
      index_keyword(schema.ExactTerm(field), value.ToString());
    }
    if (field.indexed && !columns_only) {
      VisitLeaves(
          field.type, field.name, &value,
          [&](const auto& path, const Value* leaf) {
            const auto term = leaf->ToString();
            if (field.tokenizer) {
              if (!doc.WithTokens(
                      schema.leaves.at(path), irs::StringLayout::kStrings,
                      irs::IndexFeatures::Freq | irs::IndexFeatures::Pos,
                      nullptr,
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
            index_keyword(schema.leaves.at(path), term);
          });
    }
  }
}

}  // namespace web3::searcher
