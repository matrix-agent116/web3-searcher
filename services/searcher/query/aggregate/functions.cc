#include "searcher/query/aggregate/functions.h"

#include <absl/numeric/int128.h>

#include <algorithm>
#include <core_functions/aggregate/algebraic_functions.hpp>
#include <core_functions/aggregate/distributive_functions.hpp>
#include <cstdint>
#include <duckdb/catalog/catalog.hpp>
#include <duckdb/common/types/bignum.hpp>
#include <duckdb/common/vector_operations/binary_executor.hpp>
#include <duckdb/parser/parsed_data/create_aggregate_function_info.hpp>
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace web3::searcher {
namespace {

// Divide the unsigned big-endian magnitude once per group. With a 64-bit
// count the next base-256 digit and remainder together need up to 71 bits.
duckdb::bignum_t IntegerMean(duckdb::bignum_t sum, int64_t count,
                             duckdb::Vector& result) {
  if (count <= 0) throw duckdb::InternalException("non-null sum has no rows");
  duckdb::vector<uint8_t> digits;
  bool negative;
  duckdb::Bignum::GetByteArray(digits, negative, sum.data);
  uint64_t remainder = 0;
  const auto divisor = static_cast<uint64_t>(count);
  for (auto& digit : digits) {
    const absl::uint128 dividend = (absl::uint128(remainder) << 8) | digit;
    digit = static_cast<uint8_t>(dividend / divisor);
    remainder =
        static_cast<uint64_t>(dividend - absl::uint128(digit) * divisor);
  }
  auto first = std::find_if(digits.begin(), digits.end(),
                            [](uint8_t digit) { return digit != 0; });
  if (first == digits.end())
    return duckdb::Bignum::InitializeBignumZero(result);
  auto encoded =
      duckdb::Bignum::FromByteArray(&*first, digits.end() - first, negative);
  return duckdb::bignum_t(duckdb::StringVector::AddStringOrBlob(
      result, encoded.data(), encoded.size()));
}

void FinalizeBignumMean(duckdb::DataChunk& args, duckdb::ExpressionState&,
                        duckdb::Vector& result) {
  duckdb::BinaryExecutor::Execute<duckdb::bignum_t, int64_t, duckdb::bignum_t>(
      args.data[0], args.data[1], result,
      [&](duckdb::bignum_t sum, int64_t count) {
        return IntegerMean(sum, count, result);
      });
}

template <class Function>
void Register(duckdb::Catalog& catalog, duckdb::ClientContext& context) {
  auto functions = Function::GetFunctions();
  functions.name = duckdb::Identifier(Function::Name);
  duckdb::CreateAggregateFunctionInfo info(std::move(functions));
  info.on_conflict = duckdb::OnCreateConflict::IGNORE_ON_CONFLICT;
  catalog.CreateFunction(context, info);
}

}  // namespace

void RegisterAggregateFunctions(duckdb::ClientContext& context) {
  auto& catalog = duckdb::Catalog::GetSystemCatalog(context);
  Register<duckdb::SumFun>(catalog, context);
  Register<duckdb::AvgFun>(catalog, context);
  duckdb::ScalarFunction mean(
      kBignumMean, {duckdb::LogicalType::BIGNUM, duckdb::LogicalType::BIGINT},
      duckdb::LogicalType::BIGNUM, FinalizeBignumMean);
  duckdb::CreateScalarFunctionInfo info(std::move(mean));
  catalog.CreateFunction(context, info);
}

}  // namespace web3::searcher
