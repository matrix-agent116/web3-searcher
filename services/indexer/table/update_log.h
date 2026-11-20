#ifndef WEB3_SEARCHER_SERVICES_INDEXER_TABLE_UPDATE_LOG_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_TABLE_UPDATE_LOG_H_

#include <simdjson.h>

#include <cstdint>
#include <filesystem>
#include <functional>

namespace web3::indexer {

// The manifest's last segment ID identifies the log following that checkpoint.
// All operations run under the table directory lock.
class UpdateLog {
 public:
  UpdateLog(const std::filesystem::path& table, uint64_t checkpoint);
  std::uintmax_t Size() const;
  // Parse one complete request at a time, in commit order.
  void Replay(const std::function<void(uint64_t, simdjson::dom::array,
                                       simdjson::dom::array)>& consume) const;
  // Return the new durable end offset.
  std::uintmax_t Append(uint64_t block_number,
                        simdjson::dom::array match_fields,
                        simdjson::dom::array documents,
                        std::uintmax_t offset) const;
  void Retire() const;

 private:
  std::filesystem::path path_;
};

}  // namespace web3::indexer
#endif  // WEB3_SEARCHER_SERVICES_INDEXER_TABLE_UPDATE_LOG_H_
