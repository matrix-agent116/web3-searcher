#ifndef WEB3_SEARCHER_SERVICES_INDEXER_UPDATE_LOG_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_UPDATE_LOG_H_

#include <simdjson.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>

namespace web3 {

// The manifest's last segment ID identifies the log following that checkpoint.
// All operations run under the table directory lock.
class UpdateLog {
 public:
  UpdateLog(const std::filesystem::path& table, uint64_t checkpoint);
  std::uintmax_t Size() const;
  // Parse one complete request at a time, in commit order.
  void Replay(const std::function<void(simdjson::dom::array)>& consume) const;
  void Append(std::string_view record, std::uintmax_t offset) const;
  void Retire() const;

 private:
  std::filesystem::path path_;
};

void SyncPath(const std::filesystem::path& path);

}  // namespace web3
#endif  // WEB3_SEARCHER_SERVICES_INDEXER_UPDATE_LOG_H_
