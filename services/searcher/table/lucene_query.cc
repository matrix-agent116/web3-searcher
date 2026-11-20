#include "searcher/table/lucene_query.h"

#include <algorithm>
#include <iresearch/analysis/keyword_tokenizer.hpp>
#include <iresearch/analysis/text_tokenizer.hpp>
#include <iresearch/parser/parser.hpp>
#include <iresearch/search/filters/filter_optimizer.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "searcher/config/schema.h"

namespace web3::searcher {
namespace {

// Tokenizers have mutable state. Each parse owns its query-side instances.
class QueryFields final : public irs::ParserContext::FieldProvider {
 public:
  QueryFields(const TableSchema& schema, irs::KeywordTokenizer& keyword,
              irs::OptimizeContext& optimizer)
      : schema_(schema), keyword_(keyword), optimizer_(optimizer) {}

  bool Resolve(std::string_view name,
               irs::ParserContext::Field& out) const override {
    const auto leaf = schema_.leaves.find(name);
    if (leaf == schema_.leaves.end()) return false;
    const auto field = std::ranges::find(schema_.fields, name, &Field::name);
    out = {leaf->second, irs::StringLayout::kStrings, &keyword_};
    if (field != schema_.fields.end() && field->tokenizer) {
      if (!text_) text_ = irs::analysis::TextTokenizer::Make({});
      out.tokenizer = text_.get();
    }
    // Two clauses may match different terms in a text or nested field.
    // Single-term optimizer rewrites are only valid for scalar keywords.
    if (field == schema_.fields.end() || field->type.IsNested() ||
        field->tokenizer) {
      optimizer_.analyzed_fields.insert(leaf->second);
    }
    return true;
  }

 private:
  const TableSchema& schema_;
  irs::KeywordTokenizer& keyword_;
  irs::OptimizeContext& optimizer_;
  mutable irs::analysis::Tokenizer::ptr text_;
};

}  // namespace

irs::Filter::ptr ParseLuceneQuery(const TableSchema& schema,
                                  std::string_view expression) {
  irs::KeywordTokenizer keyword;
  irs::OptimizeContext optimizer;
  QueryFields fields(schema, keyword, optimizer);
  auto root = std::make_unique<irs::BooleanFilter>();
  irs::ParserContext context{*root, irs::field_limits::invalid(),
                             irs::StringLayout::kStrings, keyword};
  context.fields = &fields;
  if (!irs::ParseQuery(context, expression)) {
    throw std::invalid_argument("invalid Lucene query: " +
                                context.error_message);
  }
  if (!root->Valid()) throw std::invalid_argument("invalid Lucene query");
  irs::Filter::ptr filter = std::move(root);
  static std::once_flag rules;
  std::call_once(rules, irs::InitOptimizeRules);
  irs::Optimize(filter, optimizer);
  return filter;
}

}  // namespace web3::searcher
