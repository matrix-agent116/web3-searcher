#include <absl/strings/str_cat.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <latch>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/http.h"
#include "common/thread_pool.h"

namespace web3::benchmark {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view kUsage =
    "usage: indexer_benchmark INDEXER_URL [--blocks N] [--tx-per-block N] "
    "[--queries N] [--query-threads N]\n"
    "defaults: 1000000 blocks, 512 transactions/block, 1 query thread "
    "searching until writes finish; --queries 0 disables search\n";

struct Options {
  std::string origin;
  uint32_t blocks = 1'000'000;
  uint32_t tx_per_block = 512;
  std::optional<uint32_t> queries = std::nullopt;
  uint32_t query_threads = 1;
};

Options ParseOptions(int argc, char** argv) {
  if (argc < 2 || argc % 2 != 0)
    throw std::invalid_argument("missing argument");
  Options options{argv[1]};
  for (int i = 2; i < argc; i += 2) {
    std::string_view key = argv[i], value = argv[i + 1];
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

// The SplitMix64 finalizer gives reproducible synthetic keys and query order.
uint64_t Mix(uint64_t n) {
  n = (n ^ (n >> 30)) * 0xbf58476d1ce4e5b9ULL;
  n = (n ^ (n >> 27)) * 0x94d049bb133111ebULL;
  return n ^ (n >> 31);
}

std::string Hash(uint64_t height, uint32_t index) {
  uint64_t prefix = Mix(height ^ Mix(index + uint64_t{1}));
  return absl::StrCat("0x", absl::Hex(prefix, absl::kZeroPad16),
                      absl::Hex(Mix(~prefix), absl::kZeroPad16),
                      absl::Hex(height, absl::kZeroPad16),
                      absl::Hex(index, absl::kZeroPad16));
}

std::string Address(uint32_t account) {
  return absl::StrCat("0x", absl::Hex(Mix(account), absl::kZeroPad16),
                      absl::Hex(Mix(~account), absl::kZeroPad16),
                      absl::Hex(account, absl::kZeroPad8));
}

std::string BlockBody(uint64_t height, uint32_t count) {
  std::string body = absl::StrCat(
      R"({"table":"transactions","match_field":["hash"],"block_number":)",
      height, R"(,"documents":[)");
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t value = Mix(Mix(height) + i);
    absl::StrAppend(
        &body, i == 0 ? "" : ",", R"({"hash":")", Hash(height, i),
        R"(","from":")", Address(value % 4096), R"(","to":)",
        value % 64 == 0 ? "null" : Quote(Address((value >> 32) % 4096)),
        R"(,"blockNumber":"0x)", absl::Hex(height), R"(","value":"0x)",
        absl::Hex(value), R"("})");
    if (body.size() + 2 > kMaxHttpBodyBytes)
      throw std::length_error(
          "block exceeds the 16 MiB HTTP limit; reduce --tx-per-block");
  }
  body += "]}";
  return body;
}

Json Request(const Options& options, const char* path, std::string body,
             simdjson::dom::parser& parser) {
  auto reply = Post(options.origin, path, std::move(body));
  if (reply.status != 200)
    throw std::runtime_error(
        absl::StrCat(path, " HTTP ", reply.status, " ", reply.body));
  auto result = parser.parse(reply.body).value();
  if (!result["ok"].get_bool().value())
    throw std::runtime_error(absl::StrCat(path, " failed: ", reply.body));
  return result;
}

uint64_t FirstBlock(const Options& options) {
  simdjson::dom::parser parser;
  auto height = Request(options, "/block_number", R"({"table":"transactions"})",
                        parser)["block_number"]
                    .value();
  if (height.is_int64() && height.get_int64().value() == -1) return 0;
  uint64_t last = height.get_uint64().value();
  if (options.blocks > std::numeric_limits<uint64_t>::max() - last)
    throw std::overflow_error("insufficient remaining block heights");
  return last + 1;
}

struct Measurements {
  // Keep seven significant bits of each nanosecond latency. The bucket's
  // upper bound is less than 1.6% above the sample, even on long runs.
  std::map<uint64_t, uint64_t> latency_buckets;
  uint64_t requests = 0;
  double seconds = 0;

  void Record(Clock::duration elapsed) {
    uint64_t ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    int shift = std::max(0, static_cast<int>(std::bit_width(ns)) - 7);
    ++latency_buckets[ns | ((uint64_t{1} << shift) - 1)];
    ++requests;
  }

  void Merge(const Measurements& other) {
    for (auto [ns, count] : other.latency_buckets) latency_buckets[ns] += count;
    requests += other.requests;
  }

  double PercentileMs(uint64_t p) const {
    uint64_t rank = (requests / 100) * p + ((requests % 100) * p + 99) / 100;
    for (auto [ns, count] : latency_buckets) {
      if (rank <= count) return ns / 1e6;
      rank -= count;
    }
    throw std::logic_error("empty latency histogram");
  }

  void Print() const {
    if (!requests) {
      std::cout << "null";
      return;
    }
    std::cout << R"({"requests":)" << requests << R"(,"seconds":)" << seconds
              << R"(,"requests_per_second":)" << requests / seconds
              << R"(,"p50_ms":)" << PercentileMs(50) << R"(,"p95_ms":)"
              << PercentileMs(95) << R"(,"p99_ms":)" << PercentileMs(99) << '}';
  }
};

struct Progress {
  std::atomic<uint32_t> started_blocks{0};
  std::atomic<uint32_t> committed_blocks{0};
  std::atomic<uint64_t> queries{0};
  std::atomic<uint64_t> overlapping_searches{0};
  std::atomic<bool> stopped{false};
  std::latch first_write{1};
};

void ReportProgress(std::stop_token stop, const Progress& progress,
                    uint32_t tx_per_block) {
  std::mutex mutex;
  std::unique_lock lock(mutex);
  std::condition_variable_any wake;
  auto previous = Clock::now();
  uint32_t last_blocks = 0;
  uint64_t last_queries = 0;
  while (!stop.stop_requested()) {
    wake.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
    if (stop.stop_requested()) break;
    auto now = Clock::now();
    double seconds = std::chrono::duration<double>(now - previous).count();
    auto blocks = progress.committed_blocks.load();
    auto queries = progress.queries.load();
    double blocks_per_second = (blocks - last_blocks) / seconds;
    std::ostringstream line;
    line << std::fixed << std::setprecision(1) << "progress blocks=" << blocks
         << " transactions=" << uint64_t{blocks} * tx_per_block
         << " queries=" << queries << " blocks/s=" << blocks_per_second
         << " tx/s=" << blocks_per_second * tx_per_block
         << " queries/s=" << (queries - last_queries) / seconds << '\n';
    std::cerr << line.str();
    previous = now;
    last_blocks = blocks;
    last_queries = queries;
  }
}

Measurements Upsert(const Options& options, uint64_t first,
                    Progress& progress) {
  Measurements stats;
  simdjson::dom::parser parser;
  auto start = Clock::now();
  for (uint32_t i = 0; i < options.blocks && !progress.stopped; ++i) {
    uint64_t height = first + i;
    auto body = BlockBody(height, options.tx_per_block);
    progress.started_blocks.store(i + 1);
    auto sent = Clock::now();
    auto result = Request(options, "/upsert", std::move(body), parser);
    if (result["block_number"].get_uint64().value() != height ||
        result["upserted"].get_uint64().value() != options.tx_per_block)
      throw std::runtime_error(
          absl::StrCat("incomplete upsert for block ", height));
    progress.committed_blocks.store(i + 1);
    if (i == 0) progress.first_write.count_down();
    stats.Record(Clock::now() - sent);
  }
  stats.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  return stats;
}

Measurements SearchWorker(const Options& options, uint64_t first,
                          uint32_t worker, uint32_t workers,
                          Progress& progress) {
  Measurements stats;
  simdjson::dom::parser parser;
  for (uint64_t i = worker; !progress.stopped; i += workers) {
    uint32_t committed = progress.committed_blocks.load();
    if (options.queries ? i >= *options.queries : committed == options.blocks)
      break;
    uint64_t row = Mix(i) % (uint64_t{committed} * options.tx_per_block);
    auto hash =
        Hash(first + row / options.tx_per_block, row % options.tx_per_block);
    auto body = absl::StrCat(
        R"({"table":"transactions","field":"hash","value":")", hash, R"("})");
    auto completed_before = progress.committed_blocks.load();
    if (!options.queries && completed_before == options.blocks) break;
    auto sent = Clock::now();
    auto hits = Request(options, "/search", std::move(body), parser)["hits"]
                    .get_array()
                    .value();
    if (hits.size() != 1 || hits.at(0)["hash"].get_string().value() != hash)
      throw std::runtime_error(
          absl::StrCat("expected one search hit for ", hash));
    auto elapsed = Clock::now() - sent;
    // A write was in flight at dispatch or started before this query finished.
    if (progress.started_blocks.load() > completed_before)
      ++progress.overlapping_searches;
    stats.Record(elapsed);
    ++progress.queries;
  }
  return stats;
}

Measurements Search(const Options& options, uint64_t first, Progress& progress,
                    ThreadPool& pool, uint32_t workers) {
  Measurements stats;
  if (!workers) return stats;
  progress.first_write.wait();
  if (progress.stopped) return stats;
  std::vector<std::future<Measurements>> tasks;
  tasks.reserve(workers);
  auto start = Clock::now();
  for (uint32_t worker = 0; worker < workers; ++worker) {
    tasks.push_back(pool.Commit([&options, &progress, first, worker, workers] {
      try {
        return SearchWorker(options, first, worker, workers, progress);
      } catch (...) {
        progress.stopped = true;
        throw;
      }
    }));
  }
  for (auto& task : tasks) stats.Merge(task.get());
  stats.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  return stats;
}

void Run(const Options& options) {
  uint64_t first = FirstBlock(options);
  uint64_t total = uint64_t{options.blocks} * options.tx_per_block;
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
      return Upsert(options, first, progress);
    } catch (...) {
      progress.stopped = true;
      if (progress.committed_blocks == 0) progress.first_write.count_down();
      throw;
    }
  });
  Measurements reads;
  try {
    reads = Search(options, first, progress, pool, readers);
  } catch (...) {
    progress.stopped = true;
    throw;
  }
  auto writes = writer.get();
  reporter.request_stop();
  reporter.join();
  std::cout << R"({"ok":true,"first_block":)" << first << R"(,"last_block":)"
            << first + (options.blocks - 1) << R"(,"transactions":)" << total
            << R"(,"transactions_per_second":)" << total / writes.seconds
            << R"(,"overlapping_searches":)"
            << progress.overlapping_searches.load() << R"(,"upsert":)";
  writes.Print();
  std::cout << R"(,"search":)";
  reads.Print();
  std::cout << "}\n";
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
