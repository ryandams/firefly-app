# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Target board and modem come from prj.conf (FIREFLY_TARGET_*, see
# Kconfig.firefly). Kconfig runs too late to add a devicetree overlay, so
# read the modem from prj.conf's text and add its fragment pair here; Zephyr
# merges these with any EXTRA_CONF_FILE/EXTRA_DTC_OVERLAY_FILE given to it.
foreach(kind BOARD MODEM)
  file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/prj.conf selected
       REGEX "^CONFIG_FIREFLY_TARGET_${kind}_[A-Z0-9_]+=y$")
  list(LENGTH selected count)
  if(NOT count EQUAL 1)
    message(FATAL_ERROR "prj.conf must set exactly one CONFIG_FIREFLY_TARGET_${kind}_*=y")
  endif()
  string(REGEX REPLACE "^CONFIG_FIREFLY_TARGET_${kind}_(.*)=y$" "\\1" firefly_${kind} "${selected}")
endforeach()

# Checked here, before devicetree: a wrong overlay fails there with a far
# less helpful message.
if(firefly_MODEM STREQUAL "NRF93M1")
  set(modem_board NRF93M1DK)
else()
  set(modem_board NRF54LM20DK)
endif()
if(NOT firefly_BOARD STREQUAL modem_board)
  message(FATAL_ERROR "prj.conf: modem ${firefly_MODEM} needs board ${modem_board}, not ${firefly_BOARD}")
endif()

string(TOLOWER "${firefly_MODEM}" modem)
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/conf/modem_${modem}.conf)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/conf/modem_${modem}.conf)
  list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/boards/${modem}.overlay)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(firefly LANGUAGES C CXX)

# The board is only known from here on (under sysbuild, BOARD is not set
# before find_package).
if(NOT CONFIG_BOARD_${firefly_BOARD})
  message(FATAL_ERROR "prj.conf selects board ${firefly_BOARD}, but this build is for ${BOARD}")
endif()
# A later conf file could still change the choice, but the overlay is fixed
# by then.
if(NOT CONFIG_FIREFLY_TARGET_MODEM_${firefly_MODEM})
  message(FATAL_ERROR "FIREFLY_TARGET_MODEM changed after prj.conf - set it only in prj.conf")
endif()

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
