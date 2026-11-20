add_executable(indexer_benchmark indexer_benchmark.cc)
target_link_libraries(indexer_benchmark PRIVATE web3_http absl::strings)
