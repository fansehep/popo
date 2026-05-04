enable_testing()

add_executable(insert_test insert_test.cc)
target_link_libraries(insert_test PRIVATE index_lib type_lib base_lib gtest gtest_main)

add_executable(insert_bench insert_bench.cc)
target_link_libraries(insert_bench PRIVATE index_lib type_lib base_lib spdlog)

include(GoogleTest)
gtest_discover_tests(insert_test)
set_target_properties(insert_test PROPERTIES COMPILE_FLAGS "-g")
set_target_properties(insert_bench PROPERTIES COMPILE_FLAGS "-g")
