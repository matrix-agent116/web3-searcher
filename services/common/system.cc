#include "common/system.h"

#ifdef __linux__
#include <sched.h>
#endif

#include <algorithm>
#include <thread>

namespace web3 {

std::size_t CpuCoreCount() noexcept {
#ifdef __linux__
  cpu_set_t cpus;
  if (sched_getaffinity(0, sizeof(cpus), &cpus) == 0) {
    int count = CPU_COUNT(&cpus);
    if (count > 0) return static_cast<std::size_t>(count);
  }
#endif
  return std::max(1u, std::thread::hardware_concurrency());
}

}  // namespace web3
