file(
    GLOB_RECURSE project_sources
    LIST_DIRECTORIES false
    "${PROJECT_ROOT}/src/*.h"
    "${PROJECT_ROOT}/src/*.cc"
    "${PROJECT_ROOT}/bench/*.h"
    "${PROJECT_ROOT}/bench/*.cc"
    "${PROJECT_ROOT}/tests/*.h"
    "${PROJECT_ROOT}/tests/*.cc"
    "${PROJECT_ROOT}/tests/*.cpp"
)

set(failed false)
foreach(source IN LISTS project_sources)
    file(READ "${source}" contents)
    if(contents MATCHES "(^|[^A-Za-z0-9_])(throw|try|catch)([^A-Za-z0-9_]|$)"
       OR contents MATCHES "std::sto(i|l|ll|d|f)[ \t\r\n]*\\("
       OR contents MATCHES "\\.value[ \t\r\n]*\\(")
        message(SEND_ERROR "${source}: use popo::base::Result<T> instead of exception-based error handling")
        set(failed true)
    endif()
endforeach()

if(failed)
    message(FATAL_ERROR "Project error-handling policy check failed")
endif()
