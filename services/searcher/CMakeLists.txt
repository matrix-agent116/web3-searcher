# The minimal IResearch build omits the tokenizer used by keyword queries.
if(IRESEARCH_BENCH)
  target_sources(iresearch PRIVATE "${CMAKE_SOURCE_DIR}/iresearch/analysis/keyword_tokenizer.cpp")
endif()

add_library(web3_searcher_table STATIC
  manager/manager.cc
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

add_library(web3_searcher_router STATIC router/internal_routes.cc router/router.cc query/query_template.cc)
target_link_libraries(web3_searcher_router PUBLIC web3_searcher_table)
add_executable(searcher main/main.cc)
target_link_libraries(searcher PRIVATE web3_searcher_router)
if(WEB3_BUILD_TESTING)
  add_executable(searcher_test
    manager/manager_test.cc
    query/query_template_test.cc
    router/router_test.cc
  )
  target_link_libraries(searcher_test PRIVATE web3_searcher_router GTest::gtest_main)
endif()
