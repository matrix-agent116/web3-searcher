#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_SESSION_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_SESSION_H_

#include <chrono>
#include <stdexcept>

namespace duckdb {
class Connection;
}

namespace web3::searcher {

class AggregateUnavailable : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Holds one concurrency slot and one deadline for the lifetime of the request.
// The pooled DuckDB connection is opened only for plans that need it.
class AggregateSession {
 public:
  AggregateSession();
  ~AggregateSession();
  AggregateSession(const AggregateSession&) = delete;
  AggregateSession& operator=(const AggregateSession&) = delete;

  void CheckDeadline() const;
  duckdb::Connection& Connection();

 private:
  struct Slot;
  static Slot& AcquireSlot();

  Slot& slot_;
  std::chrono::steady_clock::time_point deadline_;
};

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_SESSION_H_
