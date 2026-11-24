# Host unit tests of the hardware independent parts of the ESP-IDF firmware.
#   cmake -S firmware-idf/test/host -B build-host \
#         -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
#   cmake --build build-host && ctest --test-dir build-host --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(firmware_idf_host_tests C)
enable_testing()

set(main_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../main")

foreach(t json_util dns_reply)
    add_executable(test_${t} test_${t}.c "${main_dir}/${t}.c")
    target_include_directories(test_${t} PRIVATE "${main_dir}")
    set_target_properties(test_${t} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
    target_compile_options(test_${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()
