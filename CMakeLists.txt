cmake_minimum_required(VERSION 3.28)
project(web3_searcher LANGUAGES NONE)

include(ExternalProject)
set(SERENEDB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../serenedb" CACHE PATH
    "SereneDB checkout with initialized submodules")
get_filename_component(sdb "${SERENEDB_SOURCE_DIR}" REALPATH)
if(NOT EXISTS "${sdb}/iresearch/index/index_writer.hpp")
  message(FATAL_ERROR "Set SERENEDB_SOURCE_DIR to a SereneDB source checkout")
endif()
# Use upstream's external-directory hook so the executables and IResearch
# share its compiler flags, generated headers, libc++ and transitive libraries.
if(CMAKE_HOST_APPLE)
  set(web3_llvm_bin /opt/homebrew/opt/llvm/bin)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  set(web3_llvm_bin /usr/lib/llvm-23/bin)
  # Match SereneDB's Linux preset for its interdependent static archives.
  find_program(WEB3_LLD NAMES ld.lld-23 ld.lld HINTS "${web3_llvm_bin}" REQUIRED)
  set(web3_linker_args "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=${WEB3_LLD}"
                       "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=${WEB3_LLD}")
endif()
find_program(WEB3_CLANG NAMES clang-23 clang HINTS "${web3_llvm_bin}" REQUIRED)
find_program(WEB3_CLANGXX NAMES clang++-23 clang++ HINTS "${web3_llvm_bin}" REQUIRED)
set(WEB3_BUILD_JOBS 6 CACHE STRING "Concurrent jobs for the dependency build")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()
set(hv_source "${CMAKE_BINARY_DIR}/_deps/libhv-src")
ExternalProject_Add(libhv_source
  URL https://codeload.github.com/ithewei/libhv/tar.gz/refs/tags/v1.3.4
  URL_HASH SHA256=f0a9a197f90da55cc3ff104f9c7a27cc927f117b6c18613c3292726068588e10
  DOWNLOAD_NAME libhv-1.3.4.tar.gz
  DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads"
  SOURCE_DIR "${hv_source}"
  CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "")
include(CTest)
set(web3_targets searcher updater indexer_benchmark)
if(BUILD_TESTING)
  list(APPEND web3_targets http_test table_tests searcher_test)
endif()

ExternalProject_Add(serenedb
  DEPENDS libhv_source
  SOURCE_DIR "${sdb}"
  BINARY_DIR "${CMAKE_BINARY_DIR}/serenedb"
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  INSTALL_COMMAND ""
  CMAKE_ARGS
    ${web3_linker_args}
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_C_COMPILER=${WEB3_CLANG}"
    "-DCMAKE_CXX_COMPILER=${WEB3_CLANGXX}"
    "-DSDB_EXTERNAL_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/services"
    "-DWEB3_LIBHV_SOURCE_DIR=${hv_source}"
    "-DWEB3_BUILD_TESTING=${BUILD_TESTING}"
    "-DWEB3_BIN_DIR=${CMAKE_BINARY_DIR}/bin"
    -DSDB_UPDATE_SUBMODULES=OFF -DSDB_LTO=OFF -DSDB_DEBUG_INFO=NONE
    -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DSDB_WERROR=OFF -DIRESEARCH_BENCH=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_BISON=ON -DCMAKE_DISABLE_FIND_PACKAGE_FLEX=ON
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel ${WEB3_BUILD_JOBS}
                --target ${web3_targets}
  BUILD_ALWAYS ON)

add_custom_target(indexer_benchmark DEPENDS serenedb)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  function(add_searcher_test name suite)
    add_test(NAME ${name} COMMAND "${CMAKE_BINARY_DIR}/bin/searcher_test"
             "--gtest_filter=${suite}.*" --gtest_fail_if_no_test_selected)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
  endfunction()
  add_searcher_test(searcher_test QueryTemplateTest)
  add_searcher_test(aggregate_plan_test AggregatePlanTest)
  add_searcher_test(aggregate_session_test AggregateSessionTest)
  add_searcher_test(manager_test ManagerTest)
  add_searcher_test(internal_routes_test InternalRoutesTest)
  add_searcher_test(routes_test RoutesTest)
  add_test(NAME http_test COMMAND "${CMAKE_BINARY_DIR}/bin/http_test")
  function(add_table_test component filter)
    add_test(NAME ${component}_test COMMAND "${CMAKE_BINARY_DIR}/bin/table_tests"
             "--gtest_filter=${filter}" --gtest_fail_if_no_test_selected)
    set_tests_properties(${component}_test PROPERTIES LABELS table TIMEOUT 30)
  endfunction()
  add_table_test(table "TableTest.*")
  add_table_test(view_rebuilder "ViewRebuilderTest.*")
  add_table_test(candidate "CandidateTest.*")
  add_table_test(lucene_query "LuceneQueryTest.*")
  add_table_test(memory_segment "MemorySegmentTest.*")
  add_table_test(memory_segment_manager "MemorySegmentManagerTest.*")
  add_table_test(row_codec "RowCodecTest.*")
  add_table_test(segment_merger "SegmentMergerTest.*:SegmentCleanupTest.*")
  add_table_test(snapshot "SnapshotTest.*")
  add_table_test(solidity "SolidityTest.*")
  add_table_test(update_log "UpdateLogTest.*")
  add_table_test(utils "TableUtilsTest.*")
  set_tests_properties(http_test PROPERTIES TIMEOUT 30)
  add_test(NAME searcher_e2e COMMAND "${Python3_EXECUTABLE}" -B
           "${CMAKE_CURRENT_SOURCE_DIR}/e2e/searcher_routes.py" "${CMAKE_BINARY_DIR}/bin")
  add_test(NAME searcher_storage_e2e COMMAND "${Python3_EXECUTABLE}" -B
           "${CMAKE_CURRENT_SOURCE_DIR}/e2e/searcher_storage_e2e.py"
           "${CMAKE_BINARY_DIR}/bin/searcher")
  add_test(NAME searcher_e2e_fuzz COMMAND "${Python3_EXECUTABLE}" -B
           "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/searcher_fuzz.py"
           "${CMAKE_BINARY_DIR}/bin/searcher")
  set_tests_properties(searcher_e2e searcher_storage_e2e searcher_e2e_fuzz PROPERTIES LABELS e2e TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PYTHONPATH=path_list_prepend:${CMAKE_CURRENT_SOURCE_DIR}/tests")
  set_property(TEST searcher_e2e_fuzz APPEND PROPERTY LABELS fuzz)
  add_test(NAME services_smoke COMMAND "${Python3_EXECUTABLE}" -B
           "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.py" "${CMAKE_BINARY_DIR}/bin")
  set_tests_properties(services_smoke PROPERTIES TIMEOUT 90)
  add_test(NAME indexer_benchmark_parallel COMMAND "${Python3_EXECUTABLE}"
           "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_parallel.py"
           "${CMAKE_BINARY_DIR}/bin/indexer_benchmark")
  set_tests_properties(indexer_benchmark_parallel PROPERTIES TIMEOUT 20)
  add_test(NAME indexer_benchmark_retry COMMAND "${Python3_EXECUTABLE}"
           "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_retry.py"
           "${CMAKE_BINARY_DIR}/bin/indexer_benchmark")
  set_tests_properties(indexer_benchmark_retry PROPERTIES TIMEOUT 30)
endif()
