add_executable(value_test value_test.cc)
target_link_libraries(value_test PRIVATE type_lib base_lib gtest gtest_main asan spdlog)