cmake_minimum_required(VERSION 3.16)
project(Native8080 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Aggressive warnings so subtle UB surfaces early
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
)

add_executable(native8080
    src/main.cpp
    src/cpu8080.cpp
)

target_include_directories(native8080 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Debug build: keep symbols; enable sanitizers only if ASan is available.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(native8080 PRIVATE -g)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-fsanitize=address,undefined" HAS_SANITIZERS)
    if(HAS_SANITIZERS)
        target_compile_options(native8080 PRIVATE -fsanitize=address,undefined)
        target_link_options(native8080 PRIVATE -fsanitize=address,undefined)
    endif()
endif()
