# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(firefly_port_tests LANGUAGES C CXX)

set(FIREFLY_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_subdirectory(${FIREFLY_ROOT}/code/core core_build)
add_subdirectory(${FIREFLY_ROOT}/port      port_build)

target_sources(app PRIVATE src/test_hostPorts.c src/portShim.cpp)

target_include_directories(app PRIVATE
    ${FIREFLY_ROOT}/code/common
    ${FIREFLY_ROOT}/code/core
    ${FIREFLY_ROOT}/port
)

target_link_libraries(app PRIVATE firefly_core firefly_port)
