#include "benchmark/workload.h"

#include <absl/strings/str_cat.h>

#include <algorithm>
#include <stdexcept>

namespace web3::benchmark {
namespace {

// The SplitMix64 finalizer gives reproducible synthetic keys and query order.
uint64_t Mix(uint64_t n) {
  n = (n ^ (n >> 30)) * 0xbf58476d1ce4e5b9ULL;
  n = (n ^ (n >> 27)) * 0x94d049bb133111ebULL;
  return n ^ (n >> 31);
}

std::string Hash(uint64_t height, uint32_t index) {
  uint64_t prefix = Mix(height ^ Mix(index + uint64_t{1}));
  return absl::StrCat("0x", absl::Hex(prefix, absl::kZeroPad16),
                      absl::Hex(Mix(~prefix), absl::kZeroPad16),
                      absl::Hex(height, absl::kZeroPad16),
                      absl::Hex(index, absl::kZeroPad16));
}

std::string Address(uint32_t account) {
  return absl::StrCat("0x", absl::Hex(Mix(account), absl::kZeroPad16),
                      absl::Hex(Mix(~account), absl::kZeroPad16),
                      absl::Hex(account, absl::kZeroPad8));
}

uint64_t TransactionValue(uint64_t height, uint32_t index) {
  return Mix(Mix(height) + index);
}

bool HasRecipient(uint64_t value) { return value % 64 != 0; }

std::string Recipient(uint64_t value) { return Address((value >> 32) % 4096); }

bool ValidStats(simdjson::dom::array hits, bool expect_hit) {
  if (hits.size() != 1) return false;
  auto stats = hits.at(0).value();
  const auto transfers = stats["transfer_count"].get_uint64().value();
  const auto values = stats["value_count"].get_uint64().value();
  if (values > transfers || (expect_hit && !values)) return false;
  for (auto field : {"min_wei", "max_wei", "average_wei", "total_wei"}) {
    auto amount = stats[field].value();
    if (!values) {
      if (!amount.is_null()) return false;
    } else {
      auto decimal = amount.get_string().value();
      if (decimal.empty() || !std::ranges::all_of(decimal, [](char c) {
            return c >= '0' && c <= '9';
          }))
        return false;
    }
  }
  return true;
}

bool ValidRanking(simdjson::dom::array hits, bool expect_hit) {
  if (hits.size() > 10 || (expect_hit && hits.size() == 0)) return false;
  uint64_t previous_count = 0;
  std::string_view previous_sender;
  for (auto hit : hits) {
    auto sender = hit["sender"].get_string().value();
    auto count = hit["transfer_count"].get_uint64().value();
    if (sender.empty() || !count ||
        (previous_count &&
         (count > previous_count ||
          (count == previous_count && sender <= previous_sender))))
      return false;
    previous_count = count;
    previous_sender = sender;
  }
  return true;
}

}  // namespace

std::string BlockBody(uint64_t height, uint32_t count) {
  std::string body = absl::StrCat(
      R"({"table":"transactions","match_field":["hash"],"block_number":)",
      height, R"(,"documents":[)");
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t value = TransactionValue(height, i);
    absl::StrAppend(&body, i == 0 ? "" : ",", R"({"hash":")", Hash(height, i),
                    R"(","from":")", Address(value % 4096), R"(","to":)",
                    HasRecipient(value) ? Quote(Recipient(value)) : "null",
                    R"(,"blockNumber":"0x)", absl::Hex(height),
                    R"(","value":")", value, R"("})");
    if (body.size() + 2 > kMaxHttpBodyBytes)
      throw std::length_error(
          "block exceeds the 16 MiB HTTP limit; reduce --tx-per-block");
  }
  body += "]}";
  return body;
}

std::string SearchBody(std::string_view hash) {
  return absl::StrCat(R"({"table":"transactions","query":"hash:)", hash,
                      R"("})");
}

Query SampleQuery(QueryKind kind, uint64_t sequence, uint64_t first_height,
                  uint32_t visible_blocks, uint32_t tx_per_block) {
  const uint64_t rows = uint64_t{visible_blocks} * tx_per_block;
  auto row = Mix(sequence) % rows;
  auto height = first_height + row / tx_per_block;
  auto index = static_cast<uint32_t>(row % tx_per_block);
  if (kind == kHash) return {kind, Hash(height, index), true};
  auto value = TransactionValue(height, index);
  // Skip contract creations. An all-null batch still exercises the empty
  // result.
  const auto start = row;
  while (!HasRecipient(value)) {
    row = (row + 1) % rows;
    if (row == start) break;
    value =
        TransactionValue(first_height + row / tx_per_block, row % tx_per_block);
  }
  return {kind, Recipient(value), HasRecipient(value)};
}

std::string Query::Body() const {
  if (kind == kHash) return SearchBody(key);
  return absl::StrCat(R"({"address":")", key, R"("})");
}

void Query::CheckHits(simdjson::dom::array hits) const {
  bool valid;
  switch (kind) {
    case kHash:
      valid =
          hits.size() == 1 && hits.at(0)["hash"].get_string().value() == key;
      break;
    case kReceivedStats:
      valid = ValidStats(hits, expect_hit);
      break;
    case kTopSenders:
      valid = ValidRanking(hits, expect_hit);
      break;
  }
  if (!valid)
    throw std::runtime_error(absl::StrCat(
        "unexpected hits from ", kQueryRoutes[kind].path, " for ", key));
}

}  // namespace web3::benchmark
