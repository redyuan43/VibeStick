cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "../components/vibe_telemetry_common"
    "../components/vibe_telemetry_boards"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vibestick_telemetry_sticks3)
