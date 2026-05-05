enable_testing()
include(GoogleTest)

add_executable(opcode_test opcode_test.cc)
target_link_libraries(opcode_test PRIVATE bytecode_lib type_lib base_lib gtest gtest_main spdlog)
gtest_discover_tests(opcode_test)

add_executable(program_test program_test.cc)
target_link_libraries(program_test PRIVATE bytecode_lib type_lib base_lib gtest gtest_main spdlog)
gtest_discover_tests(program_test)

add_executable(compiler_test compiler_test.cc)
target_link_libraries(compiler_test PRIVATE bytecode_lib storage_lib type_lib catalog_lib base_lib parser_lib planner_lib gtest gtest_main spdlog)
gtest_discover_tests(compiler_test)

add_executable(vm_test vm_test.cc)
target_link_libraries(vm_test PRIVATE bytecode_lib storage_lib type_lib catalog_lib base_lib parser_lib planner_lib gtest gtest_main spdlog)
gtest_discover_tests(vm_test)
