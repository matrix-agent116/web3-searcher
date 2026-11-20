#include "searcher/query/query_template.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <set>
#include <stdexcept>

namespace web3::searcher {
namespace {

bool IdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

std::string Setting(Json object, const char* key, std::string fallback) {
  auto item = object[key];
  if (item.error() == simdjson::NO_SUCH_FIELD) return fallback;
  return std::string(item.get_string().value());
}

// IResearch decodes Unicode escapes after lexing a term. Escaping the first
// ASCII byte also keeps words such as AND and TO from becoming operators.
std::string LuceneTerm(std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string term;
  for (unsigned char c : value) {
    if (c < 128 && (term.empty() || !IdentChar(c))) {
      term += "\\u00";
      term += hex[c >> 4];
      term += hex[c & 15];
    } else {
      term += static_cast<char>(c);
    }
    if (term.size() > kMaxHttpBodyBytes)
      throw std::length_error("query too large");
  }
  return term;
}

bool ValidPointer(std::string_view path) {
  if (!path.starts_with('/')) return false;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '~' &&
        (++i == path.size() || (path[i] != '0' && path[i] != '1')))
      return false;
  }
  return true;
}

bool TermEnd(char c) {
  return c == ')' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

}  // namespace

QueryTemplate::QueryTemplate(Json route) {
  auto parameters = route["params"];
  const bool infer_params = parameters.error() == simdjson::NO_SUCH_FIELD;
  if (!infer_params) ReadParameters(parameters.get_object().value());
  const auto query_template = route["query"].value();
  limit_ = SearchLimit::FromJson(query_template);
  Compile(query_template["lucene"].get_string().value(), infer_params);
  ReadSelection(query_template["select"].get_object().value());
}

void QueryTemplate::ReadParameters(simdjson::dom::object params) {
  for (auto [key, spec] : params) {
    if (key.empty() || !std::ranges::all_of(key, IdentChar) ||
        std::ranges::find(params_, key, &Param::name) != params_.end())
      throw std::invalid_argument("invalid or duplicate parameter name");
    std::string name(key);
    Param param{name, Setting(spec, "type", "string"),
                Setting(spec, "get", name), Setting(spec, "post", "/" + name)};
    if (param.type != "string" && param.type != "integer" &&
        param.type != "boolean")
      throw std::invalid_argument("unknown parameter type: " + name);
    auto required = spec["required"];
    if (required.error() != simdjson::NO_SUCH_FIELD &&
        !required.get_bool().value())
      throw std::invalid_argument("query parameters must be required");
    if (param.get_key.empty() || !ValidPointer(param.post_pointer))
      throw std::invalid_argument("invalid parameter source: " + name);
    params_.push_back(std::move(param));
  }
}

void QueryTemplate::Compile(std::string_view expression, bool infer_params) {
  if (expression.empty()) throw std::invalid_argument("empty Lucene template");
  std::vector<bool> used(params_.size());
  size_t start = 0;
  char close = 0;
  for (size_t i = 0; i < expression.size(); ++i) {
    const char c = expression[i];
    if (expression.substr(i).starts_with("${")) {
      const auto end = expression.find('}', i + 2);
      if (end == expression.npos || close || i < 2 ||
          expression[i - 1] != ':' || !IdentChar(expression[i - 2]) ||
          (end + 1 < expression.size() && !TermEnd(expression[end + 1])))
        throw std::invalid_argument(
            "placeholder must be a complete field value");
      auto name = expression.substr(i + 2, end - i - 2);
      if (name.empty() || !std::ranges::all_of(name, IdentChar))
        throw std::invalid_argument("invalid placeholder name");
      auto param = std::ranges::find(params_, name, &Param::name);
      if (param == params_.end()) {
        if (!infer_params)
          throw std::invalid_argument("undeclared query parameter: " +
                                      std::string(name));
        std::string key(name);
        params_.push_back({key, "string", key, "/" + key});
        used.push_back(false);
        param = std::prev(params_.end());
      }
      auto index = static_cast<size_t>(param - params_.begin());
      used[index] = true;
      parts_.emplace_back(std::string(expression.substr(start, i - start)));
      parts_.emplace_back(index);
      i = end;
      start = end + 1;
    } else if (c == '\\') {
      ++i;
    } else if (close) {
      if (c == close ||
          ((close == ']' || close == '}') && (c == ']' || c == '}')))
        close = 0;
    } else if (c == '"' || c == '/') {
      close = c;
    } else if (c == '[' || c == '{') {
      close = c == '[' ? ']' : '}';
    }
  }
  if (std::ranges::find(used, false) != used.end())
    throw std::invalid_argument("unused query parameter");
  parts_.emplace_back(std::string(expression.substr(start)));
}

void QueryTemplate::ReadSelection(simdjson::dom::object selection) {
  std::set<std::string_view> aliases;
  std::set<std::string> columns;
  for (auto [alias, field] : selection) {
    const std::string name(field.get_string().value());
    if (alias.empty() || name.empty() || !aliases.insert(alias).second)
      throw std::invalid_argument("invalid query projection");
    select_.push_back(
        {(select_.empty() ? "" : ",") + Quote(alias) + ":", name});
    if (columns.insert(name).second) columns_.push_back(name);
  }
  if (select_.empty())
    throw std::invalid_argument("select must contain at least one field");
}

std::string QueryTemplate::Param::Bind(Json input, bool get) const {
  auto item = get ? input[get_key] : input.at_pointer(post_pointer);
  auto invalid = [&] {
    return std::invalid_argument("invalid or missing parameter: " +
                                 (get ? get_key : post_pointer));
  };
  if (item.error()) throw invalid();
  if (get || type == "string") {
    std::string_view value;
    if (item.get_string().get(value) || value.empty()) throw invalid();
    if (type == "string") return LuceneTerm(value);
    if (type == "integer") {
      int64_t number;
      auto [end, ec] =
          std::from_chars(value.data(), value.data() + value.size(), number);
      if (ec == std::errc{} && end == value.data() + value.size())
        return LuceneTerm(std::to_string(number));
    } else if (value == "true" || value == "false") {
      return LuceneTerm(value);
    }
  } else if (type == "integer") {
    int64_t number;
    if (!item.get_int64().get(number))
      return LuceneTerm(std::to_string(number));
  } else {
    bool value;
    if (!item.get_bool().get(value))
      return LuceneTerm(value ? "true" : "false");
  }
  throw invalid();
}

std::string QueryTemplate::Bind(Json input, bool get) const {
  std::vector<std::string> values;
  for (const auto& param : params_) values.push_back(param.Bind(input, get));
  std::string query;
  for (const auto& part : parts_) {
    auto* literal = std::get_if<std::string>(&part);
    const auto& text = literal ? *literal : values[std::get<size_t>(part)];
    if (text.size() > kMaxHttpBodyBytes - query.size())
      throw std::length_error("query too large");
    query += text;
  }
  return query;
}

std::vector<std::string_view> QueryTemplate::Fields() const {
  return {columns_.begin(), columns_.end()};
}

std::string QueryTemplate::Response(Json hits) const {
  std::string result = R"({"ok":true,"hits":[)";
  bool first = true;
  for (auto row : hits.get_array().value()) {
    if (!first) result += ',';
    first = false;
    result += '{';
    for (const auto& [key, column] : select_) {
      auto value = simdjson::minify(row[column].value());
      if (result.size() + key.size() + value.size() + 3 > kMaxHttpBodyBytes)
        throw std::length_error("response too large");
      result += key;
      result += value;
    }
    result += '}';
  }
  return result + "]}";
}

}  // namespace web3::searcher
