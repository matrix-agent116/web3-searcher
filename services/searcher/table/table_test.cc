#include "searcher/table/table.h"

#include <absl/cleanup/cleanup.h>
#include <absl/strings/str_cat.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "searcher/table/test_utils.h"

namespace web3::searcher {
namespace {

class TableTest : public test::TempDirectoryFixture {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(TempDirectoryFixture::SetUp());
    Open();
  }

  void TearDown() override {
    table_.reset();
    TempDirectoryFixture::TearDown();
  }

  void Open() {
    std::size_t seeded = 0;
    table_ = Table::Open(path_, db_.instance, test::MakeSchema(), seeded);
  }

  std::size_t Write(bool candidate, uint64_t height, std::string_view rows,
                    std::string_view match = R"(["id"])") {
    simdjson::dom::parser keys, docs;
    auto fields = test::ParseArray(keys, match);
    auto data = test::ParseArray(docs, rows);
    return candidate ? table_->SubmitCandidate(height, fields, data)
                     : table_->ConfirmBlock(height, fields, data);
  }

  std::vector<std::string> Values(std::string_view tag) {
    simdjson::dom::parser parser;
    auto hits = parser.parse(table_->Search(absl::StrCat("tag:", tag)))["hits"]
                    .get_array()
                    .value();
    std::vector<std::string> values;
    for (auto row : hits)
      values.emplace_back(row["value"].get_string().value());
    return values;
  }

  duckdb::DuckDB db_{nullptr};
  std::shared_ptr<Table> table_;
};

TEST_F(TableTest,
       FailedFinalizationPreservesCandidateAndEmptyFinalizationRemovesIt) {
  Write(true, 0, R"([{"id":"a","tag":"same","value":"candidate"}])");
  Write(true, 1, R"([{"id":"b","tag":"same","value":"later"}])");
  const auto pending = path_ / "transactions" / "segments.pending";
  ASSERT_TRUE(std::filesystem::create_directory(pending));
  EXPECT_THROW(Write(false, 0, R"([{"id":"a"},{"id":"b"}])"), std::exception);
  EXPECT_EQ(table_->GetBlockNumber(), -1);
  EXPECT_EQ(Values("same"), (std::vector<std::string>{"candidate", "later"}));
  ASSERT_TRUE(std::filesystem::remove(pending));
  Write(false, 0, "[]");
  EXPECT_EQ(table_->GetBlockNumber(), 0);
  EXPECT_EQ(Write(true, 0, R"([{"id":"stale"}])"), 0);
  EXPECT_EQ(Values("same"), (std::vector<std::string>{"later"}));
  EXPECT_THROW(Write(false, 0, R"([{"id":"extra"}])"), std::invalid_argument);
  table_.reset();
  Open();
  EXPECT_EQ(table_->GetBlockNumber(), 0);
  EXPECT_TRUE(Values("same").empty());
}

TEST_F(TableTest,
       EachSearchSeesOneWholeGenerationDuringReplacementAndFinalization) {
  auto block = [](int generation) {
    auto key = std::to_string(generation);
    return "[{\"id\":\"a" + key + "\",\"tag\":\"all\",\"value\":\"" + key +
           "\"},{\"id\":\"b" + key + "\",\"tag\":\"all\",\"value\":\"" + key +
           "\"}]";
  };
  Write(true, 1, block(0));
  std::promise<void> started;
  std::atomic<bool> done{false};
  auto reader = std::async(std::launch::async, [&] {
    started.set_value();
    do {
      auto values = Values("all");
      if (values.size() != 2 || values[0] != values[1]) return false;
    } while (!done.load());
    return true;
  });
  absl::Cleanup stop_reader = [&] { done.store(true); };
  started.get_future().wait();
  for (int i = 1; i < 32; ++i) Write(true, 1, block(i));
  Write(false, 1, block(32));
  done.store(true);
  EXPECT_TRUE(reader.get());
  EXPECT_EQ(Values("all"), (std::vector<std::string>{"32", "32"}));
}

// Hold a candidate build after Solidity has observed another writer's commit.
class PausingTokenizer final
    : public irs::analysis::TypedTokenizer<PausingTokenizer> {
 public:
  static constexpr std::string_view type_name() noexcept {
    return "test_pause";
  }
  irs::TokenTraits Traits() const noexcept override { return {}; }
  template <irs::TokenLayout Layout>
  bool DoFill(duckdb::string_t, irs::TokenSink&) {
    entered.set_value();
    resume.get_future().wait();
    return true;
  }
  std::promise<void> entered, resume;
};

TEST_F(TableTest, ReadsPublishedReloadWhileNextCandidateIsBuilding) {
  using namespace std::chrono_literals;
  auto schema = test::MakeSchema();
  auto tokenizer = std::make_unique<PausingTokenizer>();
  auto* gate = tokenizer.get();
  schema.fields.push_back({"gate", duckdb::LogicalType::VARCHAR, false, true, 4,
                           std::move(tokenizer)});
  schema.leaves.emplace("gate", 4);
  std::size_t seeded = 0;
  table_ = Table::Open(path_, db_.instance, std::move(schema), seeded);
  Write(true, 1, R"([{"id":"a","tag":"all","value":"candidate"}])");
  auto other = Table::Open(path_, db_.instance, test::MakeSchema(), seeded);
  simdjson::dom::parser keys, docs;
  other->ConfirmBlock(
      1, test::ParseArray(keys, R"(["id"])"),
      test::ParseArray(docs,
                       R"([{"id":"b","tag":"all","value":"confirmed"}])"));
  auto entered = gate->entered.get_future();
  auto write = std::async(std::launch::async, [&] {
    return Write(true, 2,
                 R"([{"id":"c","tag":"all","value":"next","gate":"pause"}])");
  });
  if (entered.wait_for(3s) != std::future_status::ready) {
    gate->resume.set_value();
    FAIL() << "candidate did not enter its tokenizer";
  }
  auto query = std::async(std::launch::async, [&] {
    return std::pair{table_->GetBlockNumber(), Values("all")};
  });
  auto status = query.wait_for(1s);
  gate->resume.set_value();
  ASSERT_EQ(status, std::future_status::ready);
  auto [height, rows] = query.get();
  EXPECT_EQ(height, 1);
  EXPECT_EQ(rows, (std::vector<std::string>{"confirmed"}));
  EXPECT_EQ(write.get(), 1);
  EXPECT_EQ(Values("all"), (std::vector<std::string>{"confirmed", "next"}));
}

TEST_F(TableTest, ReloadingAnotherWritersCommitRebasesRemainingCandidates) {
  Write(true, 1, R"([{"id":"a","tag":"all","value":"old"}])");
  Write(true, 2, R"([{"id":"b","tag":"all","value":"later"}])");
  std::size_t seeded = 0;
  auto other = Table::Open(path_, db_.instance, test::MakeSchema(), seeded);
  simdjson::dom::parser keys, docs;
  other->ConfirmBlock(
      1, test::ParseArray(keys, R"(["id"])"),
      test::ParseArray(docs, R"([{"id":"c","tag":"all","value":"final"}])"));
  // Solidity observes the other writer under the directory lock before
  // rejecting the duplicate.
  EXPECT_THROW(Write(false, 1, "[]"), std::invalid_argument);
  EXPECT_EQ(table_->GetBlockNumber(), 1);
  EXPECT_EQ(Values("all"), (std::vector<std::string>{"final", "later"}));
}

TEST_F(TableTest, DiskMergerPublishesReloadWithSurvivingCandidates) {
  using namespace std::chrono_literals;
  Write(false, 1, R"([{"id":"a"},{"id":"b"}])");
  Write(false, 2, R"([{"id":"c"},{"id":"d"}])");
  Write(true, 3, R"([{"id":"e","tag":"all","value":"obsolete"}])");
  Write(true, 4, R"([{"id":"f","tag":"all","value":"later"}])");
  std::size_t seeded = 0;
  auto other = Table::Open(path_, db_.instance, test::MakeSchema(), seeded);
  simdjson::dom::parser keys, docs;
  other->ConfirmBlock(
      3, test::ParseArray(keys, R"(["id"])"),
      test::ParseArray(
          docs, R"([{"id":"e","tag":"all","value":"confirmed"},{"id":"g"}])"));
  EXPECT_EQ(table_->GetBlockNumber(), 2);
  auto merger = Table::StartMerger(std::array{table_});
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (table_->GetBlockNumber() != 3) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_EQ(Values("all"), (std::vector<std::string>{"confirmed", "later"}));
  // The worker can finish compaction after Table detaches its listener.
  table_.reset();
}

TEST_F(TableTest, AuthoritativeBlockCanInvalidateAmbiguousLaterCandidate) {
  Write(true, 2, R"([{"id":"x","tag":"shared","value":"pending"}])",
        R"(["tag"])");
  Write(
      false, 1,
      R"([{"id":"a","tag":"shared","value":"a"},{"id":"b","tag":"shared","value":"b"}])");
  EXPECT_EQ(table_->GetBlockNumber(), 1);
  EXPECT_EQ(Values("shared"), (std::vector<std::string>{"a", "b"}));
  EXPECT_THROW(Write(true, 2, R"([{"id":"x","tag":"shared"}])", R"(["tag"])"),
               AmbiguousMatch);
  Write(true, 2, R"([{"id":"a","tag":"updated","value":"new"}])");
  EXPECT_EQ(Values("shared"), (std::vector<std::string>{"b"}));
  EXPECT_EQ(Values("updated"), (std::vector<std::string>{"new"}));
}

TEST_F(TableTest, AppendingUsesRebasedViewAndRejectsWholeAmbiguousBlock) {
  Write(true, 1, R"([{"id":"a","tag":"old"}])");
  Write(true, 2, R"([{"id":"b","tag":"shared","value":"pending"}])");
  Write(false, 1, R"([{"id":"a","tag":"shared","value":"final"}])");
  EXPECT_THROW(
      Write(true, 3, R"([{"id":"c","tag":"extra"},{"id":"d","tag":"shared"}])",
            R"(["tag"])"),
      AmbiguousMatch);
  EXPECT_TRUE(Values("extra").empty());
  Write(true, 3, R"([{"id":"b","tag":"new","value":"replacement"}])");
  EXPECT_EQ(Values("shared"), (std::vector<std::string>{"final"}));
  EXPECT_EQ(Values("new"), (std::vector<std::string>{"replacement"}));
  EXPECT_TRUE(Values("old").empty());
  EXPECT_EQ(table_->GetBlockNumber(), 1);
}

TEST_F(TableTest, DiskCompactionPreservesCandidatePatchesUntilConfirmation) {
  using namespace std::chrono_literals;
  Write(false, 1,
        R"([{"id":"a","tag":"x","value":"old"},{"id":"b","tag":"y"}])");
  Write(false, 2,
        R"([{"id":"a","tag":"x","value":"solid"},{"id":"c","tag":"z"}])");
  Write(
      true, 3,
      R"([{"id":"a","tag":"x","value":"candidate"},{"id":"c","tag":"moved"}])");
  const auto path = path_ / "transactions";
  const auto before = ReadManifest(path);
  {
    auto merger = Table::StartMerger(std::array{table_});
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (ReadManifest(path) == before) {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline);
      std::this_thread::sleep_for(5ms);
    }
  }
  EXPECT_EQ(table_->GetBlockNumber(), 2);
  EXPECT_EQ(Values("x"), std::vector<std::string>{"candidate"});
  EXPECT_TRUE(Values("z").empty());
  Write(true, 4, R"([{"id":"d","tag":"future","value":"later"}])");
  Write(false, 3, R"([{"id":"a","tag":"x","value":"final"}])");
  EXPECT_EQ(Values("x"), std::vector<std::string>{"final"});
  EXPECT_EQ(Values("future"), std::vector<std::string>{"later"});
  EXPECT_TRUE(Values("moved").empty());
  table_.reset();
  Open();
  EXPECT_EQ(table_->GetBlockNumber(), 3);
  EXPECT_EQ(Values("x"), std::vector<std::string>{"final"});
  EXPECT_TRUE(Values("future").empty());
}

}  // namespace
}  // namespace web3::searcher
