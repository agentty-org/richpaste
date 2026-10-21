cmake_minimum_required(VERSION 3.25)
project(richpaste LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(RICHPASTE_STATIC "Link fully static so one binary runs anywhere" OFF)
option(RICHPASTE_TESTS "Build tests" ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(RICHPASTE_STATIC)
  add_link_options(-static)
endif()

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
  src/composer.cpp
  src/term.cpp
)
target_include_directories(rp PUBLIC include)
if(WIN32)
  target_compile_definitions(rp PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
  target_link_libraries(rp PUBLIC ws2_32 bcrypt shell32 iphlpapi advapi32)
endif()

add_executable(richpaste src/main.cpp)
target_link_libraries(richpaste PRIVATE rp)

if(RICHPASTE_TESTS)
  enable_testing()
  add_executable(rp_tests tests/unit.cpp)
  target_link_libraries(rp_tests PRIVATE rp)
  add_test(NAME unit COMMAND rp_tests)
  add_executable(png_tool tests/png_tool.cpp)
  target_link_libraries(png_tool PRIVATE rp)
  if(NOT WIN32)
    add_executable(x11_owner tests/x11_owner.cpp)
    target_link_libraries(x11_owner PRIVATE rp)
  else()
    add_executable(win_setclip tests/win_setclip.cpp)
    target_link_libraries(win_setclip PRIVATE rp user32)
  endif()
endif()
