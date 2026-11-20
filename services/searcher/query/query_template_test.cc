#include "searcher/query/query_template.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace web3::searcher {
namespace {

Json Parse(simdjson::dom::parser& parser, std::string_view text) {
  return parser.parse(text.data(), text.size()).value();
}

TEST(QueryTemplateTest, BindsTypedGetAndJsonPointerPostToTheSameQuery) {
  simdjson::dom::parser parser;
  QueryTemplate query_template(Parse(parser, R"({
    "params": {"h":{"type":"integer","get":"height","post":"/a~1b/height"},
               "live":{"type":"boolean"}},
    "query":{"lucene":"height:${h} AND live:${live}","select":{"id":"hash"}}})"));
  const auto get = query_template.Bind(
      Parse(parser, R"({"height":"042","live":"true"})"), true);
  const auto post = query_template.Bind(
      Parse(parser, R"({"a/b":{"height":42},"live":true})"), false);
  EXPECT_EQ(get, post);
  EXPECT_THROW(query_template.Bind(
                   Parse(parser, R"({"height":"42x","live":"true"})"), true),
               std::invalid_argument);
  EXPECT_THROW(
      query_template.Bind(
          Parse(parser, R"({"a/b":{"height":"42"},"live":true})"), false),
      std::invalid_argument);
  EXPECT_THROW(query_template.Bind(
                   Parse(parser, R"({"height":"42","live":"maybe"})"), true),
               std::invalid_argument);
}

TEST(QueryTemplateTest, RejectsDuplicateOutputAliases) {
  simdjson::dom::parser parser;
  EXPECT_THROW(
      {
        QueryTemplate query_template(Parse(
            parser,
            R"({"query":{"lucene":"*:*","select":{"a":"value","a":"hash"}}})"));
      },
      std::invalid_argument);
}

TEST(QueryTemplateTest, RejectsMisplacedOrInvalidPlaceholders) {
  simdjson::dom::parser parser;
  for (auto lucene :
       {"id:${}", "${key}:value", "id:${key}*", "id:\"x:${key}\"",
        "id:/x:${key}/", "id:[x:${key} TO z]", "id:${unknown-name}"}) {
    std::string spec =
        R"({"query":{"select":{"id":"key"},"lucene":)" + Quote(lucene) + "}}";
    EXPECT_THROW(
        { QueryTemplate query_template(Parse(parser, spec)); },
        std::invalid_argument)
        << lucene;
  }
}

}  // namespace
}  // namespace web3::searcher
