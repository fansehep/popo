aux_source_directory(type BASE_SRC)
add_library(
  base_lib
  STATIC
  ${BASE_SRC}
)
target_include_directories(
  base_lib
  PUBLIC
  base
  ${CMAKE_SOURCE_DIR}
)
# jemalloc replaces malloc. --as-needed would drop the DSO because no
# translation unit names a jemalloc symbol.
find_library(POPO_JEMALLOC NAMES jemalloc REQUIRED)
target_link_libraries(
        base_lib
        PUBLIC
        cpptrace::cpptrace
        -Wl,--push-state,--no-as-needed
        ${POPO_JEMALLOC}
        -Wl,--pop-state
)


aux_source_directory(type TYPE_SRC)
add_library(
  type_lib
  STATIC
  ${TYPE_SRC}
)
target_include_directories(
  type_lib
  PUBLIC
  type
  ${CMAKE_SOURCE_DIR}
)

target_link_libraries(
  type_lib
  base_lib
)

aux_source_directory(catalog CATALOG_SRC)
add_library(
  catalog_lib
  STATIC
  ${CATALOG_SRC}
)
target_include_directories(
  catalog_lib
  PUBLIC
  type
  ${CMAKE_SOURCE_DIR}
)

target_link_libraries(
  catalog_lib
  base_lib
)

# Index library - now only includes header files
add_library(
  index_lib
  INTERFACE
)
target_include_directories(
  index_lib
  INTERFACE
  index
  ${CMAKE_SOURCE_DIR}
)
target_link_libraries(
  index_lib
  INTERFACE
  type_lib
  spdlog
)

aux_source_directory(expression EXPRESSION_SRC)
add_library(
  expression_lib
  STATIC
  ${EXPRESSION_SRC}
)
target_include_directories(
  expression_lib
  PUBLIC
  expression
  ${CMAKE_SOURCE_DIR}
)
target_link_libraries(
  expression_lib
  type_lib
  base_lib
)

add_library(parser_lib
    parser/lexer.cc
    parser/parser.cc
)
target_link_libraries(parser_lib PUBLIC base_lib type_lib)
target_include_directories(parser_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_library(planner_lib
    planner/planner.cc
)
target_link_libraries(planner_lib PUBLIC base_lib type_lib catalog_lib parser_lib)
target_include_directories(planner_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_library(bytecode_lib
    bytecode/program.cc
    bytecode/compiler.cc
    bytecode/vm.cc
)
target_link_libraries(bytecode_lib PUBLIC base_lib type_lib catalog_lib parser_lib planner_lib storage_lib)
target_include_directories(bytecode_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)

# Storage library (includes transaction manager)
# Table pages and the commit log are written through io_uring.
find_library(POPO_URING NAMES uring REQUIRED)
add_library(storage_lib
    storage/disk_manager.cc
    storage/io_ring.cc
    storage/row_serializer.cc
    storage/lru_replacer.cc
    storage/buffer_pool.cc
    storage/redo_log.cc
    storage/table_heap.cc
    storage/storage_engine.cc
    trancation/manager.cc
)
target_include_directories(storage_lib PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(storage_lib PUBLIC base_lib type_lib catalog_lib spdlog zstd PRIVATE ${POPO_URING})

# Add server library
add_library(server_lib
    server/protocol.cc
    server/session.cc
    server/server.cc
)
target_link_libraries(server_lib PUBLIC base_lib type_lib catalog_lib parser_lib planner_lib bytecode_lib storage_lib asio_lib spdlog)
target_include_directories(server_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)

# Add popodb-server executable
add_executable(popodb-server server/main.cc)
target_link_libraries(popodb-server PRIVATE server_lib)