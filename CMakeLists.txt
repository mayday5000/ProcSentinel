cmake_minimum_required(VERSION 3.15)
project(ProcMonTestSystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
    ${CMAKE_CURRENT_SOURCE_DIR}/usermode
)

add_executable(procmon_unified_tests
    mock_tests/main_test_runner.cpp
)

if(MSVC)
    target_compile_options(procmon_unified_tests PRIVATE /W4 /std:c++latest)
else()
    target_compile_options(procmon_unified_tests PRIVATE -Wall -Wextra)
endif()
