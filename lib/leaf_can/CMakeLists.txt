# leaf_can - builds as an ESP-IDF component inside an IDF project, or as a plain
# static library (plus host unit tests) everywhere else.
if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/leaf_can_checksum.c"
             "src/leaf_can_signals.c"
             "src/leaf_inverter_upgrade.c"
        INCLUDE_DIRS "src")
    # Defaults selectable in menuconfig (see Kconfig in this directory).
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        LEAF_TORQUE_MULTIPLIER_110KW_PERMILLE=${CONFIG_LEAF_CAN_TORQUE_110KW_PERMILLE}u
        LEAF_TORQUE_MULTIPLIER_160KW_PERMILLE=${CONFIG_LEAF_CAN_TORQUE_160KW_PERMILLE}u
        LEAF_REGEN_MULTIPLIER_PERMILLE=${CONFIG_LEAF_CAN_REGEN_PERMILLE}u
        LEAF_MIN_SOC_PERMILLE_FOR_BOOST_DEFAULT=${CONFIG_LEAF_CAN_MIN_SOC_PERMILLE}u)
    if(CONFIG_LEAF_CAN_EMULATE_1F2_GROUP)
        target_compile_definitions(${COMPONENT_LIB} PUBLIC LEAF_EMULATE_1F2_GROUP_DEFAULT=1)
    endif()
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(leaf_can C CXX)

add_library(leaf_can STATIC
    src/leaf_can_checksum.c
    src/leaf_can_signals.c
    src/leaf_inverter_upgrade.c)
target_include_directories(leaf_can PUBLIC src)
target_compile_features(leaf_can PUBLIC c_std_99)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(leaf_can PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(test)
endif()
