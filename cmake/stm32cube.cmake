# SPDX-License-Identifier: Apache-2.0
#
# STM32Cube vendor driver pack.
#
# The silicon vendor's own code is used wherever it exists rather than
# reimplemented: CMSIS-Core for the Cortex-M intrinsics, ST's CMSIS device
# layer for the register definitions, startup file and system_stm32*.c, and
# the STM32Cube HAL for clock, GPIO and UART bring-up.
#
# ST publishes these as standalone repositories, which is what the Cube
# "modular" distribution is meant for. They are fetched at configure time;
# point STM32CUBE_LOCAL_DIR at a directory holding checkouts named
# cmsis_core / cmsis_device_<fam> / stm32<fam>xx_hal_driver to build
# offline.
#
# Include this with the family set, e.g.
#
#   set(STM32CUBE_FAMILY f7)
#   include(${CMAKE_SOURCE_DIR}/cmake/stm32cube.cmake)

include(FetchContent)

if(NOT STM32CUBE_FAMILY)
    set(STM32CUBE_FAMILY "f4")
endif()

set(STM32CUBE_CMSIS_CORE_TAG   "v5.9.0_20250520" CACHE STRING "CMSIS-Core tag")
set(STM32CUBE_DEVICE_F4_TAG    "v2.6.11"         CACHE STRING "CMSIS device F4 tag")
set(STM32CUBE_HAL_F4_TAG       "v1.8.5"          CACHE STRING "STM32F4 HAL tag")
set(STM32CUBE_DEVICE_F7_TAG    "v1.2.9"          CACHE STRING "CMSIS device F7 tag")
set(STM32CUBE_HAL_F7_TAG       "v1.3.2"          CACHE STRING "STM32F7 HAL tag")
set(STM32CUBE_LOCAL_DIR        ""                CACHE PATH   "Offline Cube checkouts")

function(_cube_declare name repo tag)
    if(STM32CUBE_LOCAL_DIR AND EXISTS "${STM32CUBE_LOCAL_DIR}/${name}")
        message(STATUS "STM32Cube: using local ${name}")
        FetchContent_Declare(${name} SOURCE_DIR "${STM32CUBE_LOCAL_DIR}/${name}")
    else()
        FetchContent_Declare(${name}
            GIT_REPOSITORY "https://github.com/STMicroelectronics/${repo}.git"
            GIT_TAG        "${tag}"
            GIT_SHALLOW    TRUE
            GIT_PROGRESS   TRUE)
    endif()
endfunction()

string(TOUPPER "${STM32CUBE_FAMILY}" _fam_up)

_cube_declare(cmsis_core "cmsis_core" "${STM32CUBE_CMSIS_CORE_TAG}")
_cube_declare("cmsis_device_${STM32CUBE_FAMILY}"
              "cmsis_device_${STM32CUBE_FAMILY}"
              "${STM32CUBE_DEVICE_${_fam_up}_TAG}")
_cube_declare("stm32${STM32CUBE_FAMILY}xx_hal_driver"
              "stm32${STM32CUBE_FAMILY}xx_hal_driver"
              "${STM32CUBE_HAL_${_fam_up}_TAG}")

# Populate only; none of these ship a usable CMakeLists, so the targets are
# defined by the platform instead of via add_subdirectory.
FetchContent_MakeAvailable(
    cmsis_core
    "cmsis_device_${STM32CUBE_FAMILY}"
    "stm32${STM32CUBE_FAMILY}xx_hal_driver")

# This file is include()d, which does not create a new scope, so a plain
# set() is what makes these visible to the including CMakeLists. Using
# PARENT_SCOPE here would push them one directory level too far up and
# leave the caller with empty strings.
set(CMSIS_CORE_DIR   "${cmsis_core_SOURCE_DIR}")
set(CMSIS_DEVICE_DIR "${cmsis_device_${STM32CUBE_FAMILY}_SOURCE_DIR}")
set(STM32_HAL_DIR    "${stm32${STM32CUBE_FAMILY}xx_hal_driver_SOURCE_DIR}")
