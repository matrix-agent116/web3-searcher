#include "indexer/table/table.h"

#include <cerrno>
#include <cstdlib>
#include <system_error>
#include <utility>
#include <vector>

#include "indexer/table/utils.h"

namespace web3::indexer {
namespace fs = std::filesystem;
namespace {

std::size_t CreateTable(const fs::path& directory,
                        duckdb::DatabaseInstance& database,
                        const TableSchema& schema) {
  simdjson::dom::parser parser;
  auto rows = schema.documents
                  ? parser.load((directory / *schema.documents).string())
                  : parser.parse("[]", 2);
  auto documents = rows.get_array().value();
  // Publish a complete table in one rename; retries never open a partial index.
  std::string temp = (directory / ".table.XXXXXX").string();
  if (!mkdtemp(temp.data())) {
    throw std::system_error(errno, std::generic_category(),
                            "create staging directory");
  }
  try {
    Solidity::Write(temp, database, schema, documents);
    fs::rename(temp, directory / schema.name);
  } catch (...) {
    fs::remove_all(temp);
    throw;
  }
  return documents.size();
}

}  // namespace

Table::Table(const fs::path& path,
             duckdb::shared_ptr<duckdb::DatabaseInstance> database,
             TableSchema schema)
    : solidity_(std::make_shared<Solidity>(path, database, std::move(schema))),
      candidate_(*database, solidity_->Schema()),
      views_(solidity_->ReadSnapshot()) {
  solidity_->SetSnapshotListener(
      [this](auto base) { return PrepareConfirmedView(std::move(base)); });
}

// The disk merger may own Solidity longer than Table. Detach under Solidity's
// writer lock, which also waits for any in-flight preparation and publication.
Table::~Table() { solidity_->SetSnapshotListener({}); }

std::shared_ptr<Table> Table::Open(
    const fs::path& root, duckdb::shared_ptr<duckdb::DatabaseInstance> database,
    TableSchema schema, std::size_t& seeded) {
  auto lock = LockDirectory(root);
  const auto path = root / schema.name;
  if (!fs::exists(path) || (fs::is_directory(path) && fs::is_empty(path))) {
    seeded += CreateTable(root, *database, schema);
  }
  return std::shared_ptr<Table>(
      new Table(path, std::move(database), std::move(schema)));
}

std::unique_ptr<SegmentMerger> Table::StartMerger(
    std::span<const std::shared_ptr<Table>> tables) {
  std::vector<std::shared_ptr<Solidity>> stores;
  stores.reserve(tables.size());
  for (const auto& table : tables) stores.push_back(table->solidity_);
  return std::make_unique<SegmentMerger>(std::move(stores));
}

Solidity::OnCommit Table::PrepareConfirmedView(
    std::shared_ptr<const Snapshot> confirmed) {
  auto next = views_.Prepare([&] {
    auto overlay = candidate_.Overlay(*confirmed);
    return overlay ? overlay : confirmed;
  });
  return [this, height = confirmed->BlockNumber(),
          next = std::move(next)]() mutable noexcept {
    views_.Publish(std::move(next));
    if (height >= 0) candidate_.DiscardThrough(static_cast<uint64_t>(height));
  };
}

std::size_t Table::ConfirmBlock(uint64_t height, simdjson::dom::array fields,
                                simdjson::dom::array documents) {
  solidity_->Upsert(height, fields, documents);
  return documents.size();
}

std::size_t Table::SubmitCandidate(uint64_t height, simdjson::dom::array fields,
                                   simdjson::dom::array documents) {
  std::size_t accepted = 0;
  solidity_->WithCurrentSnapshot([&](const auto& base) {
    if (height <= base->BlockNumber()) return;
    auto next = views_.Prepare([&] {
      auto overlay = candidate_.Replace(*base, height, fields, documents);
      return overlay ? overlay : base;
    });
    views_.Publish(std::move(next));
    accepted = documents.size();
  });
  return accepted;
}

absl::int128 Table::GetBlockNumber() const {
  return views_.Read()->BlockNumber();
}

std::string Table::Search(std::string_view field,
                          std::string_view value) const {
  return views_.Read()->Search(solidity_->Schema(), field, value);
}

}  // namespace web3::indexer
