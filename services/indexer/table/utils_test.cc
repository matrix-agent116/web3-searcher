#include "indexer/table/utils.h"

#include <gtest/gtest.h>
#include <sys/file.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iresearch/store/mmap_directory.hpp>
#include <stdexcept>
#include <string>

#include "indexer/table/test_utils.h"

namespace web3::indexer {
namespace {

using TableUtilsTest = test::TempDirectoryFixture;

TEST_F(TableUtilsTest, ParsesPhysicalSegmentIdsAndRejectsMalformedNames) {
  EXPECT_EQ(SegmentId("8000000000000003"), kCompactedBase | 3);
  EXPECT_EQ(SegmentId("000000000000000a.col"), 10);
  EXPECT_THROW(SegmentId("segments"), std::runtime_error);
  EXPECT_THROW(SegmentId("000000000000000x"), std::runtime_error);
}

TEST_F(TableUtilsTest, ExcludesOtherWritersUntilReleased) {
  DirectoryLock other(std::fopen(path_.c_str(), "r"), std::fclose);
  ASSERT_NE(other, nullptr);
  {
    auto lock = LockDirectory(path_);
    const int result = ::flock(fileno(other.get()), LOCK_EX | LOCK_NB);
    const int error = errno;
    EXPECT_EQ(result, -1);
    EXPECT_EQ(error, EWOULDBLOCK);
  }
  EXPECT_EQ(::flock(fileno(other.get()), LOCK_EX | LOCK_NB), 0);
}

TEST_F(TableUtilsTest, OnlyReplacementPublishesHeightAndSegmentsTogether) {
  irs::MMapDirectory dir(path_);
  WriteManifest(dir, "segments", {});
  const SolidityManifest next{42, "1 3 2\n2 3 1\n"};
  WriteManifest(dir, "segments.pending", next);
  const auto current = ReadManifest(path_);
  EXPECT_EQ(current.block_number, -1);
  EXPECT_TRUE(current.segments.empty());
  ReplaceManifest(dir, path_, next);
  auto published = ReadManifest(path_);
  EXPECT_EQ(published.block_number, 42);
  EXPECT_EQ(published.segments, "1 3 2\n2 3 1\n");
  EXPECT_FALSE(std::filesystem::exists(path_ / "segments.pending"));
}

TEST_F(TableUtilsTest, RejectsMissingMalformedAndOutOfRangeHeights) {
  EXPECT_THROW(ReadManifest(path_), std::runtime_error);
  for (const char* header :
       {"", "block_number ", "block_number 8", "block_number -2\n",
        "block_number 12x\n", "block_number 18446744073709551616\n"}) {
    SCOPED_TRACE(header);
    std::ofstream(path_ / "segments") << header;
    EXPECT_THROW(ReadManifest(path_), std::runtime_error);
  }
}

}  // namespace
}  // namespace web3::indexer
