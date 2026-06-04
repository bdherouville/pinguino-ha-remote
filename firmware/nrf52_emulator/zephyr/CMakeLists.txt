# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Make our out-of-tree nice_nano board discoverable.
set(BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ganymede_emulator C)

target_sources(app PRIVATE src/main.c)

# clone_addr.h holds the BLE address the emulator impersonates (your remote's real
# address). It is git-ignored; if it's absent (e.g. CI / a fresh clone), fall back to
# the tracked placeholder template so the build still succeeds. A quoted
# `#include "clone_addr.h"` finds src/clone_addr.h first when present, else this copy.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/clone_addr.h)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/clone_addr.h.example
                 ${CMAKE_CURRENT_BINARY_DIR}/clone_addr_fallback/clone_addr.h COPYONLY)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/clone_addr_fallback)
  message(WARNING "clone_addr.h not found — building with the PLACEHOLDER address "
                  "from clone_addr.h.example; set src/clone_addr.h to your remote's address.")
endif()
