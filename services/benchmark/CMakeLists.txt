add_executable(indexer_benchmark indexer_benchmark.cc measurements.cc workload.cc)
target_link_libraries(indexer_benchmark PRIVATE web3_http absl::cleanup absl::strings)
