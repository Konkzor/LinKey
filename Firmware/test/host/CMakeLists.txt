cmake_minimum_required(VERSION 3.16)
project(linkey_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

include(FetchContent)
FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.6.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(unity)

set(LINKEY_MAIN_DIR ${CMAKE_SOURCE_DIR}/../../main)

enable_testing()

add_executable(test_voltage_state
    test_voltage_state.c
    ${LINKEY_MAIN_DIR}/voltage_state.c
)
target_include_directories(test_voltage_state PRIVATE ${LINKEY_MAIN_DIR})
target_link_libraries(test_voltage_state PRIVATE unity)
add_test(NAME voltage_state COMMAND test_voltage_state)
