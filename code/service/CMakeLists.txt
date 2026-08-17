# SPDX-License-Identifier: Apache-2.0
#
# firefly_service - srv* threads. A plain static library, NOT
# zephyr_library_named(): that helper only works in Zephyr *module* mode, and
# calling it from add_subdirectory() in application mode makes Zephyr warn and
# quietly not treat the result as a Zephyr library.
#
# Unlike core/, this layer does include <zephyr/...>, so it needs
# zephyr_interface unconditionally.

add_library(firefly_service STATIC)
target_sources(firefly_service PRIVATE srvController.cpp srvNetwork.cpp
    srvModemTest_nrf93m1.cpp)

target_include_directories(firefly_service PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/..
)

target_compile_features(firefly_service PUBLIC cxx_std_20)

target_link_libraries(firefly_service PRIVATE zephyr_interface PUBLIC firefly_helpers)

# Build-ORDER dependency, separate from the include/flags dependency above.
#
# zephyr_library() targets get this for free. A plain add_library() does not,
# so ninja is free to compile our sources before Zephyr has generated its
# headers - which shows up as a random "zephyr/heap_constants.h: No such file
# or directory" on some runs and not others, depending on job scheduling.
if(TARGET zephyr_generated_headers)
    add_dependencies(firefly_service zephyr_generated_headers)
endif()
