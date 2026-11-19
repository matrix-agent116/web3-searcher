add_executable(indexer main.cc schema.cc table.cc table_manager.cc update_log.cc)
target_link_libraries(indexer PRIVATE web3_http)
target_link_libraries(indexer PRIVATE iresearch)
