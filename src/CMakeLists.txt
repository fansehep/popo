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
target_link_libraries(
        base_lib
        cpptrace::cpptrace
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

