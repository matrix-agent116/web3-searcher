#include "searcher/query/aggregate/session.h"

#include <gtest/gtest.h>

namespace web3::searcher {
namespace {

TEST(AggregateSessionTest, ReleasesOnlyItsOwnSlotWhenARequestFails) {
  AggregateSession first;
  auto* first_connection = &first.Connection();
  duckdb::Connection* second_connection = nullptr;
  EXPECT_THROW(
      {
        AggregateSession second;
        second_connection = &second.Connection();
        EXPECT_NE(first_connection, second_connection);
        EXPECT_THROW((void)AggregateSession{}, AggregateUnavailable);
        throw std::invalid_argument("failed request");
      },
      std::invalid_argument);

  AggregateSession next;
  EXPECT_EQ(&next.Connection(), second_connection);
  EXPECT_THROW((void)AggregateSession{}, AggregateUnavailable);
}

}  // namespace
}  // namespace web3::searcher
