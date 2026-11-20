#include "benchmark/measurements.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace web3::benchmark {

// One stdio write keeps concurrent workers' status lines together.
void WriteStatus(std::string_view line) {
  std::fwrite(line.data(), 1, line.size(), stderr);
}

void Measurements::Record(Clock::duration elapsed) {
  seconds += std::chrono::duration<double>(elapsed).count();
  uint64_t ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  // Keep seven significant bits. Bucket bounds overestimate latency by less
  // than 1.6%.
  int shift = std::max(0, static_cast<int>(std::bit_width(ns)) - 7);
  ++latency_buckets[ns | ((uint64_t{1} << shift) - 1)];
  max_ns = std::max(max_ns, ns);
  over_one_second += elapsed > std::chrono::seconds(1);
  ++requests;
}

void Measurements::Merge(const Measurements& other) {
  for (auto [ns, count] : other.latency_buckets) latency_buckets[ns] += count;
  requests += other.requests;
  max_ns = std::max(max_ns, other.max_ns);
  over_one_second += other.over_one_second;
}

double Measurements::PercentileMs(uint64_t p) const {
  uint64_t rank = (requests / 100) * p + ((requests % 100) * p + 99) / 100;
  for (auto [ns, count] : latency_buckets) {
    if (rank <= count) return ns / 1e6;
    rank -= count;
  }
  throw std::logic_error("empty latency histogram");
}

void Measurements::Print() const {
  if (!requests) {
    std::cout << "null";
    return;
  }
  std::cout << R"({"requests":)" << requests << R"(,"seconds":)" << seconds
            << R"(,"requests_per_second":)" << requests / seconds
            << R"(,"p50_ms":)" << PercentileMs(50) << R"(,"p95_ms":)"
            << PercentileMs(95) << R"(,"p99_ms":)" << PercentileMs(99)
            << R"(,"max_ms":)" << max_ns / 1e6 << R"(,"over_1s":)"
            << over_one_second << '}';
}

void Measurements::PrintQuerySummary(std::string_view name) const {
  if (!requests) return;
  std::ostringstream line;
  line << std::fixed << std::setprecision(3) << "query_summary type=" << name
       << " requests=" << requests << " qps=" << requests / seconds
       << " p50_ms=" << PercentileMs(50) << " p95_ms=" << PercentileMs(95)
       << " p99_ms=" << PercentileMs(99) << '\n';
  WriteStatus(line.view());
}

}  // namespace web3::benchmark
