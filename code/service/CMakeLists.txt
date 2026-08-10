# SPDX-License-Identifier: Apache-2.0
#
# firefly_service - srv* threads. A plain static library, NOT
# zephyr_library_named(): that helper only works in Zephyr *module* mode, and
# calling it from add_subdirectory() in application mode makes Zephyr warn and
# quietly not treat the result as a Zephyr library.
#
# Unlike core/, this layer does include <zephyr/...>, so it needs
# zephyr_interface unconditionally.

add_library(firefly_service STATIC
    srvController.cpp
)

target_include_directories(firefly_service PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/..
)

target_compile_features(firefly_service PUBLIC cxx_std_20)

target_link_libraries(firefly_service PUBLIC  firefly_core firefly_port)
target_link_libraries(firefly_service PRIVATE zephyr_interface)
