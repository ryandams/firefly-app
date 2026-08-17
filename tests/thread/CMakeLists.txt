# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(firefly_thread_tests LANGUAGES C CXX)
target_sources(app PRIVATE src/test_thread.cpp)
target_include_directories(app PRIVATE ../../code)
