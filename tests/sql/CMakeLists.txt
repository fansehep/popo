enable_testing()
include(GoogleTest)

add_executable(sql_test sql_test.cc)
target_link_libraries(sql_test PRIVATE bytecode_lib storage_lib type_lib catalog_lib base_lib parser_lib planner_lib gtest gtest_main spdlog)
target_include_directories(sql_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
gtest_discover_tests(sql_test)

add_executable(sql_bench sql_bench.cc)
target_link_libraries(sql_bench PRIVATE bytecode_lib storage_lib type_lib catalog_lib base_lib parser_lib planner_lib spdlog)
target_include_directories(sql_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(oltp_bench oltp_bench.cc)
target_link_libraries(oltp_bench PRIVATE bytecode_lib storage_lib type_lib catalog_lib base_lib parser_lib planner_lib spdlog)
target_include_directories(oltp_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
