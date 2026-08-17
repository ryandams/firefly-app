# SPDX-License-Identifier: Apache-2.0
add_library(firefly_helpers STATIC)
target_sources(firefly_helpers PRIVATE myLeds.cpp myButtonListener.cpp)
target_include_directories(firefly_helpers PUBLIC ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/..)
target_compile_features(firefly_helpers PUBLIC cxx_std_20)
target_link_libraries(firefly_helpers PRIVATE zephyr_interface)
if(TARGET zephyr_generated_headers)
    add_dependencies(firefly_helpers zephyr_generated_headers)
endif()
