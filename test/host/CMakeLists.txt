# Host unit tests for the firmware's hardware-independent code (CI "Tests" workflow).
# The firmware sources in main/ are compiled unchanged for Linux against small stand-ins
# for the ESP-IDF headers they use (idf_stubs/, fake_idf.cpp: in-memory NVS, partition
# table, captured log). Crypto is the host's mbed TLS (libmbedtls-dev). The harness is
# SimplrSSH's (github.com/MicroHealthLLC/SimplrSSH).
#
#   cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host
#
# Two suites, run by separate CI jobs: "unit" and "persistence" (settings partition choice,
# the vault, the radio's settings and its Bluetooth devices across reboots).
cmake_minimum_required(VERSION 3.16)
project(simplrradio_host_tests C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(MAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../main)

option(STRICT "Treat warnings as errors" ON)
add_compile_options(-Wall -Wextra -Wformat=2 -fno-common)
if(STRICT)
    add_compile_options(-Werror)
endif()
option(SANITIZE "Build with AddressSanitizer and UBSan" ON)
if(SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

find_library(MBEDCRYPTO mbedcrypto REQUIRED)

# Firmware sources under test, compiled exactly as they are in main/
add_library(firmware STATIC
    ${MAIN_DIR}/bt_store.cpp
    ${MAIN_DIR}/hid_keymap.c
    ${MAIN_DIR}/hid_report.c
    ${MAIN_DIR}/portal.cpp
    ${MAIN_DIR}/secret_vault.cpp
    ${MAIN_DIR}/settings.cpp
    ${MAIN_DIR}/settings_nvs.cpp
    ${MAIN_DIR}/tab5_keymap.c
    ${MAIN_DIR}/tdeck_keymap.c
    ${MAIN_DIR}/text.c
    ${MAIN_DIR}/ui_page.cpp
    fake_idf.cpp
)
# The stand-ins come first so they shadow nothing else; main/ is the firmware's own
target_include_directories(firmware PUBLIC idf_stubs ${MAIN_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(firmware PUBLIC ${MBEDCRYPTO})

add_executable(unit_tests test_main.cpp
    tests/test_hid_keymap.cpp
    tests/test_hid_report.cpp
    tests/test_portal.cpp
    tests/test_tab5_keymap.cpp
    tests/test_tdeck_keymap.cpp
    tests/test_text.cpp
    tests/test_ui_page.cpp
)
target_link_libraries(unit_tests firmware)

add_executable(persistence_tests test_main.cpp
    tests/test_bt_store.cpp
    tests/test_secret_vault.cpp
    tests/test_settings.cpp
    tests/test_settings_nvs.cpp
)
target_link_libraries(persistence_tests firmware)

enable_testing()
add_test(NAME unit COMMAND unit_tests)
set_tests_properties(unit PROPERTIES LABELS unit)
add_test(NAME persistence COMMAND persistence_tests)
set_tests_properties(persistence PROPERTIES LABELS persistence)
