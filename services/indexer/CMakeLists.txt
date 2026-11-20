add_library(web3_indexer_storage STATIC
  config/schema.cc
  storage/memory_segment.cc
  storage/memory_segment_manager.cc
  storage/row_codec.cc
  storage/segment_merger.cc
  storage/snapshot.cc
  storage/storage.cc
  storage/update_log.cc
  storage/utils.cc
)
target_link_libraries(web3_indexer_storage PUBLIC web3_http iresearch)
add_executable(indexer main/main.cc manager/manager.cc)
target_link_libraries(indexer PRIVATE web3_indexer_storage)
if(WEB3_BUILD_TESTING)
  add_executable(storage_test
    storage/memory_segment_test.cc
    storage/memory_segment_manager_test.cc
    storage/row_codec_test.cc
    storage/segment_merger_test.cc
    storage/snapshot_test.cc
    storage/storage_test.cc
    storage/update_log_test.cc
    storage/utils_test.cc
  )
  target_link_libraries(storage_test PRIVATE web3_indexer_storage GTest::gtest_main)
endif()
