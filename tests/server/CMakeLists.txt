enable_testing()
include(GoogleTest)

add_executable(auth_test auth_test.cc)
target_link_libraries(auth_test PRIVATE server_lib gtest gtest_main spdlog)
gtest_discover_tests(auth_test)

add_executable(account_test account_test.cc)
target_link_libraries(account_test PRIVATE server_lib gtest gtest_main spdlog)
gtest_discover_tests(account_test)

add_executable(server_isolation_test server_isolation_test.cc)
target_include_directories(server_isolation_test PRIVATE /usr/include/mysql /usr/include/mysql/mysql)
target_link_libraries(server_isolation_test PRIVATE
    server_lib bytecode_lib executor_lib planner_lib parser_lib
    storage_lib catalog_lib type_lib base_lib
    gtest gtest_main
    /usr/lib/libmariadb.so
    pthread
)
gtest_discover_tests(server_isolation_test)
