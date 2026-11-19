#ifndef WEB3_SEARCHER_SERVICES_INDEXER_DIRECTORY_LOCK_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_DIRECTORY_LOCK_H_

#include <sys/file.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <system_error>

namespace web3 {
using DirectoryLock = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

// All processes using a catalog check and publish its tables under this lock.
inline DirectoryLock LockDirectory(const std::filesystem::path& directory) {
  DirectoryLock file(std::fopen(directory.c_str(), "r"), std::fclose);
  if (!file) {
    throw std::system_error(errno, std::generic_category(),
                            "open index directory " + directory.string());
  }
  while (flock(fileno(file.get()), LOCK_EX) != 0) {
    if (errno != EINTR) {
      throw std::system_error(errno, std::generic_category(),
                              "lock index directory");
    }
  }
  return file;
}

}  // namespace web3
#endif  // WEB3_SEARCHER_SERVICES_INDEXER_DIRECTORY_LOCK_H_
