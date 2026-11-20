#include "indexer/storage/utils.h"

#include <absl/cleanup/cleanup.h>
#include <absl/strings/str_cat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <iresearch/index/file_names.hpp>
#include <iresearch/index/segment_list.hpp>
#include <iresearch/store/data_output.hpp>
#include <iresearch/store/directory.hpp>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace web3::indexer {
namespace fs = std::filesystem;

uint64_t SegmentId(std::string_view name) {
  uint64_t id;
  if (!irs::ParseSegmentId(name, id))
    throw std::runtime_error("invalid segment ID: " + std::string(name));
  return id;
}

irs::IndexWriter::ptr MakeIndexWriter(irs::Directory& target,
                                      duckdb::DatabaseInstance& database) {
  irs::IndexWriterOptions options;
  options.db = &database;
  options.reader_options.db = &database;
  return irs::IndexWriter::Make(target, std::move(options));
}

std::string WriteDiskSegments(
    irs::Directory& target, duckdb::DatabaseInstance& database,
    uint64_t& last_id,
    absl::FunctionRef<void(irs::IndexWriter::Transaction&)> fill_batch) {
  auto writer = MakeIndexWriter(target, database);
  writer->ReserveSegmentId(last_id);
  auto batch = writer->GetBatch();
  fill_batch(batch);

  std::string segments;
  for (const auto& segment : batch.FlushSegments(/*sync=*/true)) {
    const auto id = SegmentId(segment.meta.name);
    last_id = std::max(last_id, id);
    irs::AppendSegmentList(segments, id, segment.meta.kinds,
                           segment.meta.docs_count);
  }
  return segments;
}

DirectoryLock LockDirectory(const std::filesystem::path& directory) {
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

void SyncFd(int fd, const fs::path& path) {
  while (::fsync(fd) != 0) {
    if (errno != EINTR)
      throw std::system_error(errno, std::generic_category(), path.string());
  }
}

void SyncPath(const fs::path& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    throw std::system_error(errno, std::generic_category(), path.string());
  absl::Cleanup close = [&] { ::close(fd); };
  SyncFd(fd, path);
}

StorageManifest ReadManifest(const fs::path& directory) {
  std::ifstream in(directory / "segments");
  if (!in)
    throw std::runtime_error("missing index manifest for table: " +
                             directory.string());
  in.exceptions(std::ios::badbit);
  std::string header;
  constexpr std::string_view prefix = "block_number ";
  if (!std::getline(in, header) || !header.starts_with(prefix) || in.eof())
    throw std::runtime_error("invalid block number header in " +
                             directory.string());
  StorageManifest manifest;
  const auto height = std::string_view(header).substr(prefix.size());
  if (height != "-1") {
    uint64_t number;
    const char* end = height.data() + height.size();
    const auto [last, error] = std::from_chars(height.data(), end, number);
    if (error != std::errc{} || last != end)
      throw std::runtime_error("invalid block number in " + directory.string());
    manifest.block_number = number;
  }
  manifest.segments.assign(std::istreambuf_iterator<char>(in), {});
  return manifest;
}

void WriteManifest(irs::Directory& dir, std::string_view name,
                   const StorageManifest& manifest) {
  auto out = dir.create(name);
  const auto header =
      absl::StrCat("block_number ", manifest.block_number, "\n");
  const auto prefix = irs::ViewCast<irs::byte_type>(std::string_view(header));
  out->WriteData(prefix.data(), prefix.size());
  auto bytes =
      irs::ViewCast<irs::byte_type>(std::string_view(manifest.segments));
  out->WriteData(bytes.data(), bytes.size());
  out->Close();
}

void ReplaceManifest(irs::Directory& target, const fs::path& directory,
                     const StorageManifest& manifest) {
  WriteManifest(target, "segments.pending", manifest);
  SyncPath(directory / "segments.pending");
  fs::rename(directory / "segments.pending", directory / "segments");
}

}  // namespace web3::indexer
