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

# Fibocom LE270 modem_cellular vendor driver, kept in-repo instead of as a
# Zephyr-tree patch (unlike the EG800Z/nRF93M1 drivers, which predate this
# and still live under ncsworkspace/zephyr/drivers/modem/vendor_modem_cellular/
# - see zephyr/patches.yml). dts/bindings/modem/fibocom,le270.yaml needs no
# wiring at all: Zephyr's DTS_ROOT always includes APPLICATION_SOURCE_DIR
# (cmake/modules/pre_dt.cmake), so <app>/dts/bindings is picked up
# automatically. CONFIG_DT_HAS_FIBOCOM_LE270_ENABLED is available here
# because find_package(Zephyr ...) above already ran devicetree + Kconfig
# generation before this point in the file.
if(CONFIG_DT_HAS_FIBOCOM_LE270_ENABLED)
    target_sources(app PRIVATE drivers/modem/cellular_fibocom_le270.c)
endif()

target_link_libraries(app PRIVATE firefly_service)
