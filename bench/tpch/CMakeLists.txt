find_package(SQLite3 REQUIRED)

add_library(tpch_workload STATIC workload.cc)
target_include_directories(tpch_workload PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(tpch_workload PUBLIC base_lib)

add_executable(tpch_bench main.cc)
target_include_directories(tpch_bench PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/sql)
target_link_libraries(
    tpch_bench
    PRIVATE
    tpch_workload
    SQLite::SQLite3
    bytecode_lib
    executor_lib
    storage_lib
    type_lib
    catalog_lib
    parser_lib
    planner_lib
    spdlog
)

add_executable(tpch_workload_test workload_test.cc)
target_link_libraries(tpch_workload_test PRIVATE tpch_workload gtest gtest_main)
include(GoogleTest)
gtest_discover_tests(tpch_workload_test)

add_custom_target(tpch_workload_check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "TpchWorkload"
    DEPENDS tpch_workload_test
    COMMENT "Running focused TPC-H workload tests"
)
