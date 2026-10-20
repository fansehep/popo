# Integration tests: compare PopoDB SQL results with MySQL 8.0
add_executable(sql_integration_test sql_integration_test.cc)

target_include_directories(sql_integration_test PRIVATE /usr/include/mysql /usr/include/mysql/mysql)
target_link_libraries(sql_integration_test PRIVATE
    server_lib bytecode_lib planner_lib parser_lib
    catalog_lib type_lib base_lib expression_lib
    gtest gtest_main
    /usr/lib/libmariadb.so
    pthread
)

# Enable CTest integration
enable_testing()
add_test(NAME sql_integration_test COMMAND sql_integration_test)
