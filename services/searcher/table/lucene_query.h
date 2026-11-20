#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_LUCENE_QUERY_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_LUCENE_QUERY_H_

#include <iresearch/search/filters/filter.hpp>
#include <string_view>

namespace web3::searcher {

struct TableSchema;

irs::Filter::ptr ParseLuceneQuery(const TableSchema& schema,
                                  std::string_view expression);

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_LUCENE_QUERY_H_
