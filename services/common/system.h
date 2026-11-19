#ifndef WEB3_SEARCHER_SERVICES_COMMON_SYSTEM_H_
#define WEB3_SEARCHER_SERVICES_COMMON_SYSTEM_H_

#include <cstddef>

namespace web3 {

// Returns the logical CPU count, using the calling thread's affinity on Linux
// when available. Returns at least one, even if detection fails.
std::size_t CpuCoreCount() noexcept;

}  // namespace web3

#endif  // WEB3_SEARCHER_SERVICES_COMMON_SYSTEM_H_
