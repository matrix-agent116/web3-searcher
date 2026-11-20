#include "indexer/table/update_log.h"

#include <absl/cleanup/cleanup.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "indexer/table/utils.h"

namespace web3::indexer {
namespace fs = std::filesystem;

UpdateLog::UpdateLog(const fs::path& table, uint64_t checkpoint)
    : path_(table / ("updates." + std::to_string(checkpoint) + ".wal")) {}

std::uintmax_t UpdateLog::Size() const {
  std::error_code error;
  auto size = fs::file_size(path_, error);
  if (error == std::errc::no_such_file_or_directory) return 0;
  if (error) throw fs::filesystem_error("stat update log", path_, error);
  return size;
}

void UpdateLog::Replay(
    const std::function<void(uint64_t, simdjson::dom::array,
                             simdjson::dom::array)>& consume) const {
  if (!Size()) return;
  std::ifstream input(path_, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read " + path_.string());
  input.exceptions(std::ios::badbit);
  simdjson::dom::parser parser;
  std::string line;
  std::uintmax_t offset = 0;
  while (std::getline(input, line)) {
    if (input.eof()) {
      // A killed writer may leave an unterminated final record.
      fs::resize_file(path_, offset);
      break;
    }
    if (line.size() <= 2 || line.front() != '{' || line.back() != '}') {
      throw std::runtime_error("invalid update log record in " +
                               path_.string());
    }
    auto request = parser.parse(line).get_object().value();
    consume(request["block_number"].get_uint64().value(),
            request["match_field"].get_array().value(),
            request["documents"].get_array().value());
    offset += line.size() + 1;
  }
}

std::uintmax_t UpdateLog::Append(uint64_t block_number,
                                 simdjson::dom::array match_fields,
                                 simdjson::dom::array documents,
                                 std::uintmax_t offset) const {
  const auto record = R"({"block_number":)" + std::to_string(block_number) +
                      R"(,"match_field":)" + simdjson::minify(match_fields) +
                      R"(,"documents":)" + simdjson::minify(documents) + "}\n";
  std::string_view remaining(record);
  int fd =
      ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0)
    throw std::system_error(errno, std::generic_category(), path_.string());
  absl::Cleanup close = [&] { ::close(fd); };
  try {
    while (!remaining.empty()) {
      auto n = ::write(fd, remaining.data(), remaining.size());
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0)
        throw std::system_error(n < 0 ? errno : EIO, std::generic_category(),
                                path_.string());
      remaining.remove_prefix(n);
    }
    SyncFd(fd, path_);
    if (offset == 0) SyncPath(path_.parent_path());
  } catch (...) {
    fs::resize_file(path_, offset);
    SyncFd(fd, path_);
    throw;
  }
  return offset + record.size();
}

void UpdateLog::Retire() const {
  // Once the next manifest is durable this generation is never replayed.
  std::error_code ignored;
  fs::remove(path_, ignored);
}

}  // namespace web3::indexer
