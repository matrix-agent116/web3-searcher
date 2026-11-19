# The upstream revision assumes that size_t and uint64_t have the same
# underlying type. Let Clang read corrected copies from the build tree on Mac.
function(web3_darwin_overlay target relative_source before after)
  set(source "${CMAKE_SOURCE_DIR}/${relative_source}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
  file(READ "${source}" code)
  string(REPLACE "${before}" "${after}" patched "${code}")
  if(code STREQUAL patched)
    return()
  endif()
  get_filename_component(name "${source}" NAME)
  set(copy "${CMAKE_CURRENT_BINARY_DIR}/${name}")
  file(CONFIGURE OUTPUT "${copy}" CONTENT "${patched}" @ONLY)
  set(overlay "${copy}.json")
  file(CONFIGURE OUTPUT "${overlay}" CONTENT [=[
{"version":0,"use-external-names":false,"roots":[{"type":"file","name":"@source@","external-contents":"@copy@"}]}
]=] @ONLY)
  target_compile_options(${target} PRIVATE -ivfsoverlay "${overlay}")
endfunction()

web3_darwin_overlay(duckdb_operator_aggregate
  third_party/duckdb/src/execution/operator/aggregate/group_limit_bound.cpp
  "MaxValue(limit, keys.size())" "MaxValue<idx_t>(limit, keys.size())")
web3_darwin_overlay(iresearch iresearch/formats/index/term_dict_reader.cpp
  "std::min(inline_size, copy.size() - kSlack)"
  "std::min<uint64_t>(inline_size, copy.size() - kSlack)")
web3_darwin_overlay(duckdb_storage_table
  third_party/duckdb/src/storage/table/row_group_reorderer.cpp
  "MinValue(row_group_offset, row_group_map.size())"
  "MinValue<idx_t>(row_group_offset, row_group_map.size())")
