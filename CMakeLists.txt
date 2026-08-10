# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(firefly LANGUAGES C CXX)

# core/ and port/ are plain CMake libraries with no Zephyr dependency of their
# own; they inherit the toolchain from this project.
add_subdirectory(code/core    core_build)
add_subdirectory(port         port_build)
add_subdirectory(code/service service_build)

target_sources(app PRIVATE code/main.c)

# House style: bare-filename includes.
target_include_directories(app PRIVATE
    code
    code/common
    code/core
    code/core/helpers
    code/service
    code/utils
    port
)

target_link_libraries(app PRIVATE firefly_core firefly_port firefly_service)
