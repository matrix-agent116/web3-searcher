#ifndef WEB3_SEARCHER_SERVICES_BENCHMARK_MEASUREMENTS_H_
#define WEB3_SEARCHER_SERVICES_BENCHMARK_MEASUREMENTS_H_

#include <chrono>
#include <cstdint>
#include <map>
#include <string_view>

namespace web3::benchmark {

using Clock = std::chrono::steady_clock;

void WriteStatus(std::string_view line);

struct Measurements {
  void Record(Clock::duration elapsed);
  void Merge(const Measurements& other);
  double PercentileMs(uint64_t p) const;
  void Print() const;
  void PrintQuerySummary(std::string_view name) const;

  std::map<uint64_t, uint64_t> latency_buckets;
  uint64_t requests = 0;
  uint64_t max_ns = 0;
  uint64_t over_one_second = 0;
  double seconds = 0;
};

}  // namespace web3::benchmark

#endif  // WEB3_SEARCHER_SERVICES_BENCHMARK_MEASUREMENTS_H_
