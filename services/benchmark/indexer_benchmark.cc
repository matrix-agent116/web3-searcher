#include <absl/cleanup/cleanup.h>
#include <absl/strings/str_cat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <format>
#include <future>
#include <iostream>
#include <iterator>
#include <latch>
#include <limits>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark/measurements.h"
#include "benchmark/workload.h"
#include "common/http.h"
#include "common/thread_pool.h"

namespace web3::benchmark {
namespace {

constexpr uint64_t kConfirmationDepth = 16;
constexpr std::string_view kUsage =
    "usage: indexer_benchmark SEARCHER_URL [--blocks N] [--tx-per-block N] "
    "[--queries N] [--query-threads N] "
    "[--query-mode mixed|hash|received_stats|top_senders]\n"
    "defaults: 1000000 blocks, 512 transactions/block, 1 query thread "
    "running mixed queries until writes finish. Confirmation lags by 16 "
    "blocks.\n"
    "--queries 0 disables search\n";

struct Options {
  std::string origin;
  uint32_t blocks = 1'000'000;
  uint32_t tx_per_block = 512;
  std::optional<uint32_t> queries = std::nullopt;
  uint32_t query_threads = 1;
  std::optional<QueryKind> query_kind = std::nullopt;
};

Options ParseOptions(int argc, char** argv) {
  if (argc < 2 || argc % 2 != 0)
    throw std::invalid_argument("missing argument");
  Options options{argv[1]};
  for (int i = 2; i < argc; i += 2) {
    std::string_view key = argv[i], value = argv[i + 1];
    if (key == "--query-mode") {
      options.query_kind.reset();
      if (value != "mixed") {
        auto route = std::ranges::find(kQueryRoutes, value, &QueryRoute::name);
        if (route == kQueryRoutes.end())
          throw std::invalid_argument(
              absl::StrCat("unknown query mode: ", value));
        options.query_kind =
            static_cast<QueryKind>(route - kQueryRoutes.begin());
      }
      continue;
    }
    uint32_t number = 0;
    auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size() ||
        (number == 0 && key != "--queries"))
      throw std::invalid_argument(absl::StrCat("invalid value for ", key));
    if (key == "--blocks")
      options.blocks = number;
    else if (key == "--tx-per-block")
      options.tx_per_block = number;
    else if (key == "--queries")
      options.queries = number;
    else if (key == "--query-threads")
      options.query_threads = number;
    else
      throw std::invalid_argument(absl::StrCat("unknown option ", key));
  }
  if (options.query_threads > kMaxHttpInFlight)
    throw std::invalid_argument("query threads must be between 1 and 64");
  return options;
}

constexpr unsigned kMaxRetries = 3;

bool Retryable(const Reply& reply) {
  return reply.transport_failure || reply.status == 408 || reply.status == 504;
}

void WaitToRetry(const char* path, unsigned retry, const Reply& reply) {
  auto delay = std::chrono::milliseconds(100u << retry);
  WriteStatus(absl::StrCat(
      path, " ",
      reply.transport_failure ? "transport failure"
                              : absl::StrCat("HTTP ", reply.status),
      ", retry ", retry + 1, "/", kMaxRetries, " in ", delay.count(), " ms\n"));
  std::this_thread::sleep_for(delay);
}

Json ParseReply(const char* path, const Reply& reply,
                simdjson::dom::parser& parser) {
  if (reply.status != 200)
    throw std::runtime_error(
        absl::StrCat(path, " HTTP ", reply.status, " ", reply.body));
  auto result = parser.parse(reply.body).value();
  if (!result["ok"].get_bool().value())
    throw std::runtime_error(absl::StrCat(path, " failed: ", reply.body));
  return result;
}

Json PostWithRetry(const Options& options, const char* path,
                   const std::string& body, simdjson::dom::parser& parser) {
  for (unsigned retry = 0;; ++retry) {
    auto reply = Post(options.origin, path, body);
    if (!Retryable(reply) || retry == kMaxRetries)
      return ParseReply(path, reply, parser);
    WaitToRetry(path, retry, reply);
  }
}

std::optional<uint64_t> LastBlock(const Options& options,
                                  simdjson::dom::parser& parser) {
  auto height =
      PostWithRetry(options, "/block_number", R"({"table":"transactions"})",
                    parser)["block_number"]
          .value();
  if (height.is_int64() && height.get_int64().value() == -1)
    return std::nullopt;
  return height.get_uint64().value();
}

uint64_t FirstBlock(const Options& options) {
  simdjson::dom::parser parser;
  auto last = LastBlock(options, parser);
  if (!last) return 0;
  if (kConfirmationDepth + options.blocks >
      std::numeric_limits<uint64_t>::max() - *last)
    throw std::overflow_error("insufficient remaining block heights");
  // The first confirmation must advance the durable height by one.
  return *last + 1 + kConfirmationDepth;
}

// A lost response may follow a successful commit. Another writer could also
// have used that height, so verify all submitted rows before counting it.
bool BlockWasCommitted(const Options& options, uint64_t height,
                       const std::string& body, simdjson::dom::parser& parser) {
  auto last = LastBlock(options, parser);
  if (!last || *last < height) return false;
  if (*last > height)
    throw std::runtime_error(
        absl::StrCat("table advanced past unacknowledged block ", height));
  simdjson::dom::parser submitted;
  auto docs = submitted.parse(body)["documents"].get_array().value();
  for (auto doc : docs) {
    auto hash = doc["hash"].get_string().value();
    auto hits =
        PostWithRetry(options, "/search", SearchBody(hash), parser)["hits"]
            .get_array()
            .value();
    if (hits.size() != 1)
      throw std::runtime_error(absl::StrCat("cannot verify committed block ",
                                            height, ", hash ", hash));
    auto row = hits.at(0).get_object().value();
    for (auto field : doc.get_object().value()) {
      auto actual = row[field.key];
      if (actual.error() ||
          simdjson::minify(actual.value()) != simdjson::minify(field.value))
        throw std::runtime_error(
            absl::StrCat("different data at block ", height, ", hash ", hash));
    }
  }
  return true;
}

void CheckBlockReply(Json result, std::string_view path, uint64_t height,
                     uint32_t tx_per_block) {
  if (result["block_number"].get_uint64().value() != height ||
      result["upserted"].get_uint64().value() != tx_per_block)
    throw std::runtime_error(
        absl::StrCat(path, " incomplete for block ", height));
}

void ConfirmBlock(const Options& options, uint64_t height,
                  const std::string& body, simdjson::dom::parser& parser) {
  for (unsigned retry = 0;; ++retry) {
    auto reply = Post(options.origin, "/solidity", body);
    if (Retryable(reply)) {
      if (BlockWasCommitted(options, height, body, parser)) return;
      if (retry < kMaxRetries) {
        WaitToRetry("/solidity", retry, reply);
        continue;
      }
    } else if (retry && reply.status == 400) {
      // The original request can finish after the height check but before
      // the retry takes the table's writer lock.
      std::string_view error;
      if (!parser.parse(reply.body)["error"].get(error) &&
          error == "block already finalized" &&
          BlockWasCommitted(options, height, body, parser))
        return;
    }
    auto result = ParseReply("/solidity", reply, parser);
    CheckBlockReply(result, "/solidity", height, options.tx_per_block);
    return;
  }
}

struct Progress {
  std::atomic<uint32_t> started_blocks{0};
  std::atomic<uint32_t> candidate_blocks{0};
  std::atomic<uint32_t> committed_blocks{0};
  std::atomic<uint32_t> completed_steps{0};
  std::atomic<uint64_t> next_query{0};
  // The two aggregate routes share two execution slots in the searcher.
  std::counting_semaphore<2> aggregate_slots{2};
  std::atomic<uint64_t> overlapping_searches{0};
  std::atomic<bool> stopped{false};
  std::latch first_visible{1};

  struct QuerySample {
    uint64_t count = 0;
    Clock::duration elapsed{};
  };
  using QuerySamples = std::array<QuerySample, kQueryRoutes.size()>;

  void RecordQuery(QueryKind kind, Clock::duration elapsed) {
    std::lock_guard lock(samples_mutex_);
    ++samples_[kind].count;
    samples_[kind].elapsed += elapsed;
  }

  QuerySamples TakeSamples() {
    std::lock_guard lock(samples_mutex_);
    return std::exchange(samples_, {});
  }

 private:
  std::mutex samples_mutex_;
  QuerySamples samples_{};
};

void ReportProgress(std::stop_token stop, Progress& progress,
                    uint32_t tx_per_block) {
  std::mutex mutex;
  std::unique_lock lock(mutex);
  std::condition_variable_any wake;
  auto previous = Clock::now();
  uint32_t last_blocks = 0;
  uint32_t last_candidates = 0;
  uint64_t total_queries = 0;
  while (!stop.stop_requested()) {
    wake.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
    if (stop.stop_requested()) break;
    auto now = Clock::now();
    double seconds = std::chrono::duration<double>(now - previous).count();
    auto blocks = progress.committed_blocks.load();
    auto candidates = progress.candidate_blocks.load();
    const auto samples = progress.TakeSamples();
    uint64_t completed = 0;
    for (const auto& sample : samples) completed += sample.count;
    total_queries += completed;
    double blocks_per_second = (blocks - last_blocks) / seconds;
    std::string report = "progress\n";
    auto out = std::back_inserter(report);
    constexpr std::string_view kColumns = "  {:<18} {:>12} {:>11}\n";
    std::format_to(out, kColumns, "metric", "total", "rate/s");
    auto write_metric = [&](std::string_view name, uint64_t total,
                            double rate) {
      std::format_to(out, "  {:<18} {:>12} {:>11.1f}\n", name, total, rate);
    };
    write_metric("candidate_blocks", candidates,
                 (candidates - last_candidates) / seconds);
    write_metric("confirmed_blocks", blocks, blocks_per_second);
    write_metric("confirmed_tx", uint64_t{blocks} * tx_per_block,
                 blocks_per_second * tx_per_block);
    write_metric("queries", total_queries, completed / seconds);

    report += '\n';
    std::format_to(out, kColumns, "query", "QPS", "avg_ms");
    for (size_t i = 0; i < samples.size(); ++i) {
      const auto& sample = samples[i];
      std::string avg_ms = "-";
      if (sample.count) {
        const double ms =
            std::chrono::duration<double, std::milli>(sample.elapsed).count();
        avg_ms = std::format("{:.3f}", ms / sample.count);
      }
      std::format_to(out, "  {:<18} {:>12.1f} {:>11}\n", kQueryRoutes[i].name,
                     sample.count / seconds, avg_ms);
    }
    report += '\n';
    WriteStatus(report);
    previous = now;
    last_blocks = blocks;
    last_candidates = candidates;
  }
}

struct WriteMeasurements {
  Measurements candidate;
  Measurements solidity;
  double seconds = 0;
};

WriteMeasurements WriteBlocks(const Options& options, uint64_t first,
                              Progress& progress) {
  WriteMeasurements stats;
  std::deque<std::string> pending;
  simdjson::dom::parser parser;
  auto start = Clock::now();
  for (uint32_t i = 0; i < options.blocks && !progress.stopped; ++i) {
    uint64_t height = first + i;
    const auto& body =
        pending.emplace_back(BlockBody(height, options.tx_per_block));
    progress.started_blocks.store(i + 1);
    auto sent = Clock::now();
    // Retrying a candidate replaces the same height with the same documents.
    auto reply = PostWithRetry(options, "/candidate", body, parser);
    CheckBlockReply(reply, "/candidate", height, options.tx_per_block);
    stats.candidate.Record(Clock::now() - sent);
    progress.candidate_blocks.store(i + 1);

    if (height >= kConfirmationDepth) {
      const uint64_t target = height - kConfirmationDepth;
      // On resume, the oldest candidates predate this run. Their synthetic
      // transactions are reproducible from the height and transaction count.
      std::string confirmed;
      if (target < first) {
        confirmed = BlockBody(target, options.tx_per_block);
      } else {
        confirmed = std::move(pending.front());
        pending.pop_front();
      }
      sent = Clock::now();
      ConfirmBlock(options, target, confirmed, parser);
      stats.solidity.Record(Clock::now() - sent);
      ++progress.committed_blocks;
    }
    progress.completed_steps.store(i + 1);
    if (i == 0) progress.first_visible.count_down();
  }
  stats.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  return stats;
}

using QueryMeasurements = std::array<Measurements, kQueryRoutes.size()>;

QueryMeasurements SearchWorker(const Options& options, uint64_t first,
                               Progress& progress) {
  QueryMeasurements stats;
  simdjson::dom::parser parser;
  while (!progress.stopped) {
    auto sequence = progress.next_query.fetch_add(1);
    if (options.queries && sequence >= *options.queries) break;
    const auto kind = options.query_kind.value_or(
        static_cast<QueryKind>(sequence % kQueryRoutes.size()));
    if (kind != kHash) progress.aggregate_slots.acquire();
    absl::Cleanup release = [&] {
      if (kind != kHash) progress.aggregate_slots.release();
    };
    const auto visible = progress.completed_steps.load();
    if (progress.stopped || (!options.queries && visible == options.blocks))
      break;
    auto query =
        SampleQuery(kind, sequence, first, visible, options.tx_per_block);
    auto body = query.Body();
    const auto completed_before = progress.completed_steps.load();
    if (!options.queries && completed_before == options.blocks) break;
    auto sent = Clock::now();
    auto hits =
        PostWithRetry(options, kQueryRoutes[kind].path, body, parser)["hits"]
            .get_array()
            .value();
    query.CheckHits(hits);
    auto elapsed = Clock::now() - sent;
    if (progress.started_blocks.load() > completed_before)
      ++progress.overlapping_searches;
    std::move(release).Invoke();
    stats[kind].Record(elapsed);
    progress.RecordQuery(kind, elapsed);
  }
  return stats;
}

struct SearchMeasurements {
  Measurements total;
  QueryMeasurements by_kind;
};

SearchMeasurements Search(const Options& options, uint64_t first,
                          Progress& progress, ThreadPool& pool,
                          uint32_t workers) {
  SearchMeasurements stats;
  if (!workers) return stats;
  progress.first_visible.wait();
  if (progress.stopped) return stats;
  std::vector<std::future<QueryMeasurements>> tasks;
  tasks.reserve(workers);
  auto start = Clock::now();
  for (uint32_t worker = 0; worker < workers; ++worker) {
    tasks.push_back(pool.Commit([&options, &progress, first] {
      try {
        return SearchWorker(options, first, progress);
      } catch (...) {
        progress.stopped = true;
        throw;
      }
    }));
  }
  for (auto& task : tasks) {
    auto samples = task.get();
    for (size_t i = 0; i < samples.size(); ++i)
      stats.by_kind[i].Merge(samples[i]);
  }
  stats.total.seconds =
      std::chrono::duration<double>(Clock::now() - start).count();
  for (auto& kind : stats.by_kind) {
    kind.seconds = stats.total.seconds;
    stats.total.Merge(kind);
  }
  return stats;
}

void Run(const Options& options) {
  uint64_t first = FirstBlock(options);
  Progress progress;
  // Reserve an HTTP request for the writer while reads and writes can overlap.
  uint32_t readers =
      std::min(options.query_threads,
               options.blocks > 1 ? kMaxHttpInFlight - 1 : kMaxHttpInFlight);
  if (options.queries) readers = std::min(readers, *options.queries);
  ThreadPool pool(readers + 1);
  std::jthread reporter([&](std::stop_token stop) {
    ReportProgress(stop, progress, options.tx_per_block);
  });
  auto writer = pool.Commit([&] {
    try {
      return WriteBlocks(options, first, progress);
    } catch (...) {
      progress.stopped = true;
      if (progress.completed_steps == 0) progress.first_visible.count_down();
      throw;
    }
  });
  SearchMeasurements reads;
  try {
    reads = Search(options, first, progress, pool, readers);
  } catch (...) {
    progress.stopped = true;
    throw;
  }
  auto writes = writer.get();
  reporter.request_stop();
  reporter.join();
  for (size_t i = 0; i < reads.by_kind.size(); ++i)
    reads.by_kind[i].PrintQuerySummary(kQueryRoutes[i].name);
  const auto last = first + (options.blocks - 1);
  const auto total = writes.solidity.requests * options.tx_per_block;
  std::cout << R"({"ok":true,"first_block":)" << first << R"(,"last_block":)"
            << last << R"(,"last_solidity_block":)"
            << (writes.solidity.requests
                    ? std::to_string(last - kConfirmationDepth)
                    : "-1")
            << R"(,"transactions":)" << total
            << R"(,"transactions_per_second":)" << total / writes.seconds
            << R"(,"overlapping_searches":)"
            << progress.overlapping_searches.load() << R"(,"candidate":)";
  writes.candidate.Print();
  std::cout << R"(,"solidity":)";
  writes.solidity.Print();
  std::cout << R"(,"search":)";
  reads.total.Print();
  std::cout << R"(,"query_types":{)";
  for (size_t i = 0; i < kQueryRoutes.size(); ++i) {
    if (i) std::cout << ',';
    std::cout << '"' << kQueryRoutes[i].name << R"(":)";
    reads.by_kind[i].Print();
  }
  std::cout << "}}\n";
}

}  // namespace
}  // namespace web3::benchmark

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << web3::benchmark::kUsage;
    return 0;
  }
  try {
    web3::benchmark::Run(web3::benchmark::ParseOptions(argc, argv));
  } catch (const std::invalid_argument& error) {
    std::cerr << error.what() << '\n' << web3::benchmark::kUsage;
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
