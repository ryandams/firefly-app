# SPDX-License-Identifier: Apache-2.0
#
# firefly_core - pure logic. NO Zephyr headers anywhere below this directory.
#
# Built twice from the same sources:
#   1. as part of the Zephyr application (hardware and native_sim)
#   2. standalone by tests/core on the host, with no Zephyr present at all
#
# The only Zephyr-awareness is picking up the toolchain flags when a Zephyr
# build happens to be the parent.

add_library(firefly_core STATIC
    helpers/myMission.cpp
    helpers/mySoc.cpp
    helpers/myReportFactory.cpp
)

# House style is bare-filename includes ("mySoc.hpp"), so every code directory
# core needs is on the include path.
target_include_directories(firefly_core PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/helpers
    ${CMAKE_CURRENT_LIST_DIR}/../common
)

target_compile_features(firefly_core PUBLIC cxx_std_20)

target_compile_options(firefly_core PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

if(TARGET zephyr_interface)
    target_link_libraries(firefly_core PRIVATE zephyr_interface)
endif()
