cmake_minimum_required(VERSION 3.25)
project(richpaste LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(RICHPASTE_STATIC "Link fully static so one binary runs on any Linux" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

add_library(rp STATIC
  src/core.cpp
  src/sha256.cpp
  src/io.cpp
  src/wire.cpp
  src/clipboard.cpp
  src/wayland.cpp
  src/x11.cpp
  src/agent.cpp
  src/composer.cpp
  src/term.cpp
)
target_include_directories(rp PUBLIC include)

add_executable(richpaste src/main.cpp)
target_link_libraries(richpaste PRIVATE rp)
if(RICHPASTE_STATIC)
  target_link_options(richpaste PRIVATE -static)
endif()

enable_testing()
add_executable(rp_tests tests/unit.cpp)
target_link_libraries(rp_tests PRIVATE rp)
add_test(NAME unit COMMAND rp_tests)

add_executable(x11_owner tests/x11_owner.cpp)
target_link_libraries(x11_owner PRIVATE rp)
