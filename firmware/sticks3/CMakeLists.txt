cmake_minimum_required(VERSION 3.16)

option(VIBE_SERIAL_DEBUG "Enable serial commands that simulate hardware buttons" OFF)
option(VIBE_RECORDING_DIAGNOSTICS
       "Persist the most recent recording transport summary for diagnosis" OFF)

if(DEFINED VIBE_BOARD AND NOT VIBE_BOARD STREQUAL "cardputer_adv")
    message(FATAL_ERROR "This experimental worktree only builds cardputer_adv")
endif()
set(VIBE_BOARD "cardputer_adv" CACHE STRING "VibeStick board target" FORCE)
set(IDF_TARGET "esp32s3")
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.cardputer_adv")
set(VIBE_FIRMWARE_VERSION_MACRO "VIBE_STICK_FIRMWARE_VERSION_CARDPUTER_ADV")
set(VIBE_STICK_CONFIG_HEADER_PATH
    "${CMAKE_CURRENT_LIST_DIR}/include/vibe_stick_config.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${VIBE_STICK_CONFIG_HEADER_PATH}")
file(READ "${VIBE_STICK_CONFIG_HEADER_PATH}" VIBE_STICK_CONFIG_HEADER)
string(REGEX MATCH
       "#define[ \t]+${VIBE_FIRMWARE_VERSION_MACRO}[ \t]+\"([^\"]+)\""
       VIBE_FIRMWARE_VERSION_MATCH "${VIBE_STICK_CONFIG_HEADER}")
if(NOT VIBE_FIRMWARE_VERSION_MATCH)
    message(FATAL_ERROR "Could not read ${VIBE_FIRMWARE_VERSION_MACRO} from vibe_stick_config.h")
endif()
set(PROJECT_VER "${CMAKE_MATCH_1}")

set(EXTRA_COMPONENT_DIRS src)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vibe_stick_cardputer_adv)
