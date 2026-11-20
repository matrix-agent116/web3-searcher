#include "searcher/query/aggregate/session.h"

#include <array>
#include <atomic>
#include <duckdb/main/config.hpp>
#include <duckdb/main/connection.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/main/settings.hpp>
#include <memory>

#include "searcher/query/aggregate/functions.h"
#include "searcher/query/aggregate/snapshot_rows.h"

namespace web3::searcher {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kTimeout = std::chrono::seconds(30);
constexpr int kConcurrency = 2;

duckdb::DuckDB& Database() {
  static const auto database = [] {
    duckdb::DBConfig config;
    config.options.maximum_threads = kConcurrency;
    config.options.maximum_memory = 256ULL << 20;
    config.options.use_temporary_directory = false;
    auto db = std::make_unique<duckdb::DuckDB>(nullptr, &config);
    duckdb::Connection connection(*db);
    auto& context = *connection.context;
    context.RunFunctionInTransaction([&] {
      RegisterAggregateFunctions(context);
      SnapshotRows::Register(context);
    });
    return db;
  }();
  return *database;
}

}  // namespace

struct AggregateSession::Slot {
  std::atomic_flag busy;
  std::unique_ptr<duckdb::Connection> connection;
};

AggregateSession::Slot& AggregateSession::AcquireSlot() {
  static std::array<Slot, kConcurrency> slots;
  for (auto& slot : slots) {
    if (!slot.busy.test_and_set(std::memory_order_acquire)) return slot;
  }
  throw AggregateUnavailable("aggregate busy");
}

AggregateSession::AggregateSession()
    : slot_(AcquireSlot()), deadline_(Clock::now() + kTimeout) {}

AggregateSession::~AggregateSession() {
  slot_.busy.clear(std::memory_order_release);
}

void AggregateSession::CheckDeadline() const {
  if (Clock::now() >= deadline_)
    throw AggregateUnavailable("aggregate timed out");
}

duckdb::Connection& AggregateSession::Connection() {
  if (!slot_.connection)
    slot_.connection = std::make_unique<duckdb::Connection>(Database());
  const auto millis =
      std::chrono::ceil<std::chrono::milliseconds>(deadline_ - Clock::now());
  if (millis.count() <= 0) throw AggregateUnavailable("aggregate timed out");
  duckdb::Settings::Set<duckdb::MaxExecutionTimeSetting>(
      *slot_.connection->context, duckdb::SetScope::SESSION,
      duckdb::Value::BIGINT(millis.count()));
  return *slot_.connection;
}

}  // namespace web3::searcher
