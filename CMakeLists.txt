cmake_minimum_required(VERSION 3.25)
project(richpaste LANGUAGES CXX)

# richpaste is agentty's clipboard library (agentty paste). It has no program
# of its own; tests/driver.cpp is a small test-only stand-in.

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
  endif()
endif()

option(RICHPASTE_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})

add_library(rp STATIC
  src/core.cpp
  src/sha256.cpp
  src/io.cpp
  src/io_posix.cpp
  src/io_win.cpp
  src/wire.cpp
  src/clipboard.cpp
  src/clip_win.cpp
  src/clip_mac.cpp
  src/wayland.cpp
  src/x11.cpp
  src/image.cpp
  src/inband.cpp
  src/armor.cpp
  src/agent.cpp
  src/store.cpp
  src/term.cpp
  src/report.cpp
  src/cli.cpp
)
target_include_directories(rp PUBLIC include)
target_compile_features(rp PUBLIC cxx_std_23)
if(PROJECT_IS_TOP_LEVEL AND NOT MSVC)
  target_compile_options(rp PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()
if(WIN32)
  # PRIVATE: UNICODE flips every A/W macro (gai_strerror -> gai_strerrorW)
  # in whatever links rp, which broke agentty's MSVC build.
  target_compile_definitions(rp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
  target_link_libraries(rp PUBLIC ws2_32 bcrypt shell32 iphlpapi advapi32 user32)
endif()
find_package(Threads REQUIRED)
target_link_libraries(rp PUBLIC Threads::Threads)
# std::print's Windows terminal path (__open_terminal / __write_to_terminal)
# lives in libstdc++'s experimental archive on mingw gcc.
if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_link_libraries(rp PUBLIC stdc++exp)
endif()

if(RICHPASTE_TESTS)
  enable_testing()
  add_executable(rp_tests tests/unit.cpp)
  target_link_libraries(rp_tests PRIVATE rp)
  add_test(NAME unit COMMAND rp_tests)
  # Stands in for agentty in the shell tests (tests/*.sh).
  add_executable(rp_driver tests/driver.cpp)
  target_link_libraries(rp_driver PRIVATE rp)
  add_executable(png_tool tests/png_tool.cpp)
  target_link_libraries(png_tool PRIVATE rp)
  if(NOT WIN32)
    add_executable(x11_owner tests/x11_owner.cpp)
    target_link_libraries(x11_owner PRIVATE rp)
  endif()
endif()
