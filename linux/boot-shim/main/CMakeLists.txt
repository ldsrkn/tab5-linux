# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only

idf_component_register(
    SRCS "main.c" "m2_sdmmc.c" "c68_secondary_entry.S"
    INCLUDE_DIRS "."
)

# Build identification (#22): the git revision is printed at boot.
if(DEFINED ENV{ES_GIT_REV} AND NOT "$ENV{ES_GIT_REV}" STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EASYSTICK_GIT_REV="$ENV{ES_GIT_REV}")
endif()

# Board (#tab5): stamp-p4 (default) or tab5.  The Tab5 powers its C6 through
# an I2C IO expander and wires it to different Slot 1 pins.
if("$ENV{EASYSTICK_BOARD}" STREQUAL "tab5")
    target_sources(${COMPONENT_LIB} PRIVATE "tab5_power.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EASYSTICK_BOARD_TAB5=1)
    # ILI9881C panel + GT911 touch: pulse TP_RST with LCD_RST and latch the
    # touch address 0x14 (the ST7123 variant has its own touch controller).
    if("$ENV{EASYSTICK_TAB5_PANEL}" STREQUAL "ili9881c")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE EASYSTICK_TAB5_TOUCH_RESET=1)
    endif()
elseif(NOT "$ENV{EASYSTICK_BOARD}" STREQUAL "" AND
       NOT "$ENV{EASYSTICK_BOARD}" STREQUAL "stamp-p4")
    message(FATAL_ERROR "unsupported EASYSTICK_BOARD: $ENV{EASYSTICK_BOARD}")
endif()

# Keep M1's proven boot path free of SDIO side effects.  The M2 and C68
# network builds opt in explicitly when they build this project.
if("$ENV{EASYSTICK_BOOT_PROFILE}" STREQUAL "m2" OR
   "$ENV{EASYSTICK_BOOT_PROFILE}" STREQUAL "c68")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EASYSTICK_M2_SDIO=1)
endif()
if("$ENV{EASYSTICK_C68_CLEAN_RELEASE}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EASYSTICK_C68_CLEAN_RELEASE=1)
elseif("$ENV{EASYSTICK_BOOT_PROFILE}" STREQUAL "m1" OR
       "$ENV{EASYSTICK_BOOT_PROFILE}" STREQUAL "m2")
    # UP Linux has CONFIG_SMP=n; do not compile the historical CPU1 probe
    # whose C33 layout symbols exist only in the C68 experiment.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EASYSTICK_UP_BOOT=1)
endif()
if("$ENV{EASYSTICK_TSENS_ORACLE}" STREQUAL "1")
    target_sources(${COMPONENT_LIB} PRIVATE "tsens_oracle.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EASYSTICK_TSENS_ORACLE=1)
endif()
if("$ENV{EASYSTICK_CMD53_RETENTION_BB}" STREQUAL "1")
    target_sources(${COMPONENT_LIB} PRIVATE "easystick_cmd53_bb.c")
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../cmd53-bb")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EASYSTICK_CMD53_RETENTION_BB=1)
    if("$ENV{EASYSTICK_CMD52_MARKER}" STREQUAL "1")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE EASYSTICK_CMD52_MARKER=1)
    endif()
    if("$ENV{EASYSTICK_CMD53_BB_SELFTEST_TORN}" STREQUAL "1")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE EASYSTICK_CMD53_BB_SELFTEST_TORN=1)
    elseif("$ENV{EASYSTICK_CMD53_BB_SELFTEST}" STREQUAL "1")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE EASYSTICK_CMD53_BB_SELFTEST=1)
    endif()
endif()
