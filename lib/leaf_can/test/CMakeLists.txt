add_executable(test_leaf_can test_leaf_can.c)
target_link_libraries(test_leaf_can PRIVATE leaf_can)
target_compile_options(test_leaf_can PRIVATE -Wall -Wextra -Werror)
add_test(NAME leaf_can_unit COMMAND test_leaf_can)

# Differential test against the original v1.3.0 handler (see reference/README.md).
add_executable(diff_original
    diff_original.cpp
    reference/can_bridge_manager_leaf.cpp
    reference/helper_functions.cpp)
target_include_directories(diff_original PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} reference reference/stubs)
set_target_properties(diff_original PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(diff_original PRIVATE -w)
target_link_libraries(diff_original PRIVATE leaf_can)
foreach(kw 110 160)
    foreach(seed 1 2 3)
        add_test(NAME diff_original_${kw}kw_seed${seed} COMMAND diff_original ${kw} ${seed})
    endforeach()
endforeach()
