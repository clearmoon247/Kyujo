# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
cmake_minimum_required(VERSION 3.16)
project(kyujo LANGUAGES CXX)

# Host build: the portable libraries and their tests. The console build (devkitA64) is added in a later milestone.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(kyujo_common STATIC src/common/config.cpp src/common/launcher_cfg.cpp)
target_include_directories(kyujo_common PUBLIC src)
target_compile_options(kyujo_common PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(test_common tests/test_common.cpp)
target_link_libraries(test_common PRIVATE kyujo_common)
target_compile_options(test_common PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME common COMMAND test_common)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME rom_tool COMMAND ${Python3_EXECUTABLE} -I -m unittest discover -s ${CMAKE_SOURCE_DIR}/tests -p test_rom_tool.py)
endif()
