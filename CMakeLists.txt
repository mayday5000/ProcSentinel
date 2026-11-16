cmake_minimum_required(VERSION 3.15)
project(ProcSentinelTestSystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
    ${CMAKE_CURRENT_SOURCE_DIR}/usermode
)

# Host unit tests (any OS)
add_executable(procsentinel_unified_tests
    mock_tests/main_test_runner.cpp
)

if(MSVC)
    target_compile_options(procsentinel_unified_tests PRIVATE /W4 /std:c++latest)
else()
    target_compile_options(procsentinel_unified_tests PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME procsentinel_unified_tests COMMAND procsentinel_unified_tests)

# User-mode client (Windows only). The kernel driver is built with MSBuild/WDK
# (driver/procsentinel_driver.vcxproj), not CMake.
if(WIN32)
    add_executable(procsentinel_app
        usermode/procsentinel_app.cpp
    )
    target_link_libraries(procsentinel_app PRIVATE advapi32)
    if(MSVC)
        target_compile_options(procsentinel_app PRIVATE /W4 /std:c++latest /EHsc)
    else()
        target_compile_options(procsentinel_app PRIVATE -Wall -Wextra)
    endif()
endif()
