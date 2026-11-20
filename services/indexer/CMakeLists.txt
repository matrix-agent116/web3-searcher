add_library(web3_indexer_table STATIC
  config/schema.cc
  table/view_rebuilder.cc
  table/table.cc
  table/candidate.cc
  table/memory_segment.cc
  table/memory_segment_manager.cc
  table/row_codec.cc
  table/segment_merger.cc
  table/snapshot.cc
  table/solidity.cc
  table/update_log.cc
  table/utils.cc
)
target_link_libraries(web3_indexer_table PUBLIC web3_http iresearch)
add_executable(indexer main/main.cc manager/manager.cc)
target_link_libraries(indexer PRIVATE web3_indexer_table)
if(WEB3_BUILD_TESTING)
  add_executable(table_tests
    table/view_rebuilder_test.cc
    table/table_test.cc
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
  target_link_libraries(table_tests PRIVATE web3_indexer_table GTest::gtest_main)
endif()
