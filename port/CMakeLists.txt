# SPDX-License-Identifier: Apache-2.0
#
# firefly_port - the HARDWARE boundary: nRF54LM20 silicon on one side, the
# host PC on the other.
#
# It is explicitly NOT an RTOS boundary. Zephyr is present in both builds -
# native_sim links the real kernel, scheduler, zbus and logging - so Zephyr is
# never something this layer abstracts away. Only peripherals are.
#
#   port/mcu   real silicon: GNSS, BLE, modem, external flash, power rails
#   port/host  the same interfaces satisfied without hardware
#
# NOTE: port/drivers, port/boards and port/dts are empty placeholders. When the
# first out-of-tree driver or the custom firefly board lands, add
# firefly-app/zephyr/module.yml with:
#     build:    { cmake: port, kconfig: port/Kconfig }
#     settings: { board_root: port, dts_root: port }
# and move the driver/binding wiring into this file.

add_library(firefly_port STATIC)

target_include_directories(firefly_port PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../code/core
    ${CMAKE_CURRENT_LIST_DIR}/../code/common
)

target_link_libraries(firefly_port PUBLIC firefly_core)
target_compile_features(firefly_port PUBLIC cxx_std_20)

if(CONFIG_FIREFLY_PORT_HOST)
    message(STATUS "firefly: HOST ports (port/host) - no silicon")
    target_sources(firefly_port PRIVATE host/hostPorts.cpp)
else()
    message(STATUS "firefly: MCU ports (port/mcu) - nRF54LM20")
    target_sources(firefly_port PRIVATE mcu/mcuPorts.cpp)
endif()

if(TARGET zephyr_interface)
    target_link_libraries(firefly_port PRIVATE zephyr_interface)
endif()
