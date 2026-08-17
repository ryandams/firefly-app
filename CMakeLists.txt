# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(firefly LANGUAGES C CXX)

add_subdirectory(code/helpers helpers_build)
add_subdirectory(code/service service_build)

target_sources(app PRIVATE code/main.c code/event_bus.c)

# House style: bare-filename includes.
target_include_directories(app PRIVATE
    code
    code/service
    code/helpers
)

target_link_libraries(app PRIVATE firefly_service)
