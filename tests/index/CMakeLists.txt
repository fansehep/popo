enable_testing()

add_executable(
  insert_test
  insert_test.cc
)
target_link_libraries(
  insert_test
  type_lib
  index_lib
  spdlog
  GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(insert_test)
set_target_properties(insert_test PROPERTIES COMPILE_FLAGS "-g")