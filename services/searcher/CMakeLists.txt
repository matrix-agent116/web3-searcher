# The minimal IResearch build omits the tokenizer used by keyword queries.
if(IRESEARCH_BENCH)
  target_sources(iresearch PRIVATE "${CMAKE_SOURCE_DIR}/iresearch/analysis/keyword_tokenizer.cpp")
endif()

add_library(web3_searcher_table STATIC
  manager/manager.cc
  query/aggregate/count.cc
  query/aggregate/executor.cc
  query/aggregate/expressions.cc
  query/aggregate/functions.cc
  query/aggregate/plan.cc
  query/aggregate/session.cc
  query/aggregate/snapshot_rows.cc
  query/row_scan.cc
  config/schema.cc
  table/view_rebuilder.cc
  table/table.cc
  table/candidate.cc
  table/lucene_query.cc
  table/memory_segment.cc
  table/memory_segment_manager.cc
  table/row_codec.cc
  table/segment_merger.cc
  table/snapshot.cc
  table/solidity.cc
  table/update_log.cc
  table/utils.cc
)
target_link_libraries(web3_searcher_table PUBLIC web3_http iresearch)
set(core_functions_dir "${CMAKE_SOURCE_DIR}/third_party/duckdb/extension/core_functions")
target_include_directories(web3_searcher_table SYSTEM PRIVATE "${core_functions_dir}/include")
if(IRESEARCH_BENCH)
  # The minimal DuckDB build omits core_functions. Reuse its two aggregates.
  target_sources(web3_searcher_table PRIVATE
    "${core_functions_dir}/aggregate/distributive/sum.cpp"
    "${core_functions_dir}/aggregate/algebraic/avg.cpp")
else()
  target_link_libraries(web3_searcher_table PRIVATE core_functions_extension)
endif()
if(WEB3_BUILD_TESTING)
  add_executable(table_tests
    table/view_rebuilder_test.cc
    table/table_test.cc
    table/lucene_query_test.cc
    table/candidate_test.cc
    table/memory_segment_test.cc
    table/memory_segment_manager_test.cc
    table/row_codec_test.cc
    table/segment_merger_test.cc
    table/snapshot_test.cc
    table/solidity_test.cc
    table/update_log_test.cc
    table/utils_test.cc
  )
  target_link_libraries(table_tests PRIVATE web3_searcher_table GTest::gtest_main)
endif()

add_library(web3_searcher_router STATIC router/internal_routes.cc router/routes.cc query/query_template.cc)
target_link_libraries(web3_searcher_router PUBLIC web3_searcher_table)
add_executable(searcher main/main.cc)
target_link_libraries(searcher PRIVATE web3_searcher_router)
if(WEB3_BUILD_TESTING)
  add_executable(searcher_test
    manager/manager_test.cc
    query/aggregate/plan_test.cc
    query/aggregate/session_test.cc
    query/query_template_test.cc
    router/internal_routes_test.cc
    router/routes_test.cc
  )
  target_link_libraries(searcher_test PRIVATE web3_searcher_router GTest::gtest_main)
endif()
