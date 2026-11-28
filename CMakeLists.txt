cmake_minimum_required(VERSION 3.20)
project(rithereal VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(RITH_WERROR "Treat warnings as errors" OFF)
option(RITH_SANITIZE "Build with address and undefined-behaviour sanitizers" OFF)

add_library(rith_core
  src/netlist.cpp
  src/sim.cpp
  src/bench.cpp
  src/verilog.cpp
  src/design.cpp
  src/scan.cpp
  src/scan_out.cpp
  src/tpi.cpp
  src/file_io.cpp
  src/liberty.cpp
  src/fault.cpp
  src/testability.cpp
  src/atpg.cpp
  src/sat.cpp)
target_include_directories(rith_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(rith_core PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(rith_core PRIVATE /W4)
else()
  target_compile_options(rith_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
  if(RITH_WERROR)
    target_compile_options(rith_core PRIVATE -Werror)
  endif()
  if(RITH_SANITIZE)
    target_compile_options(rith_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rith_core PUBLIC -fsanitize=address,undefined)
  endif()
endif()

add_executable(rith tools/rith_main.cpp)
target_link_libraries(rith PRIVATE rith_core)

include(CTest)
if(BUILD_TESTING)
  add_executable(rith_tests tests/test_main.cpp)
  target_link_libraries(rith_tests PRIVATE rith_core)
  target_compile_definitions(rith_tests PRIVATE RITH_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  add_test(NAME rith_tests COMMAND rith_tests)
endif()
