# bridge_settings - ESP-IDF component inside an IDF project, plain static library otherwise.
if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/bridge_settings.c"
        INCLUDE_DIRS "src"
        REQUIRES leaf_can)
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(bridge_settings C)

if(NOT TARGET leaf_can)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../leaf_can ${CMAKE_CURRENT_BINARY_DIR}/leaf_can)
endif()

add_library(bridge_settings STATIC src/bridge_settings.c)
target_include_directories(bridge_settings PUBLIC src)
target_link_libraries(bridge_settings PUBLIC leaf_can)
target_compile_features(bridge_settings PUBLIC c_std_99)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bridge_settings PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_executable(test_bridge_settings test/test_bridge_settings.c)
    target_link_libraries(test_bridge_settings PRIVATE bridge_settings)
    target_compile_options(test_bridge_settings PRIVATE -Wall -Wextra -Werror)
    add_test(NAME bridge_settings_unit COMMAND test_bridge_settings)
endif()
