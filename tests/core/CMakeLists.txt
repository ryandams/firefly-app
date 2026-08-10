# SPDX-License-Identifier: Apache-2.0
#
#   west twister -T firefly-app/tests -p native_sim
#
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(firefly_core_tests LANGUAGES C CXX)

set(FIREFLY_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_subdirectory(${FIREFLY_ROOT}/code/core core_build)

target_sources(app PRIVATE
    src/test_myMission.cpp
    src/test_mySoc.cpp
    src/test_myReportFactory.cpp
)

target_link_libraries(app PRIVATE firefly_core)
