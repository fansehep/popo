
add_executable(
  value_test
  value_test.cc
)
target_link_libraries(
  value_test
  type_lib
  asan
  spdlog::spdlog
)