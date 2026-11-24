# Esp32LeafInverterBridge - ESP-IDF firmware (ESP-IDF v6.1)
cmake_minimum_required(VERSION 3.22)

# Shared, hardware independent libraries (also used by the Arduino firmware).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../lib/leaf_can"
    "${CMAKE_CURRENT_LIST_DIR}/../lib/bridge_settings")

# Only build what the bridge needs.
set(COMPONENTS main)

set(PROJECT_VER "2.1.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(leaf_inverter_bridge)
