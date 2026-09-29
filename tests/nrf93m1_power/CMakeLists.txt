# SPDX-License-Identifier: Apache-2.0
#
#   west twister -T firefly-app/tests/nrf93m1_power -p <board> \
#       --device-testing --device-serial /dev/ttyACM0 --tag hardware
#
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(firefly_nrf93m1_power_tests LANGUAGES C CXX)

set(FIREFLY_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_subdirectory(${FIREFLY_ROOT}/code/helpers helpers_build)
add_subdirectory(${FIREFLY_ROOT}/code/service service_build)

target_sources(app PRIVATE
    src/test_nrf93m1_power.cpp
    ${FIREFLY_ROOT}/code/event_bus.c
)

target_link_libraries(app PRIVATE firefly_service)
