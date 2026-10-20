cmake_minimum_required(VERSION 3.16)

# Firmware version. CI sets FIRMWARE_VERSION to the release it computes (latest vX.Y.Z tag with
# the patch bumped, .github/scripts/plan_release.py). The value here is for local builds and is
# the floor for releases: raise it to jump to a new minor/major version.
if(DEFINED ENV{FIRMWARE_VERSION})
    set(PROJECT_VER "$ENV{FIRMWARE_VERSION}")
else()
    set(PROJECT_VER "2.0.0")
endif()

# Board: BOARD=tab5 (default) or BOARD=waveshare, from the environment. Both are ESP32-P4, so the
# target can't tell them apart. Each board has its own build directory, its settings
# (sdkconfig.defaults + sdkconfig.defaults.<board>, applied to the committed sdkconfig.<board>)
# and its component lock (dependencies.lock.<board>):
#   BOARD=waveshare idf.py -B build-waveshare build
# The choice is written to board_config.cmake (untracked) for main/CMakeLists.txt, which is
# evaluated by separate cmake runs that can't see -D variables.
if(DEFINED ENV{BOARD})
    set(BOARD "$ENV{BOARD}")
else()
    set(BOARD "tab5")
endif()
if(NOT BOARD STREQUAL "tab5" AND NOT BOARD STREQUAL "waveshare")
    message(FATAL_ERROR "BOARD must be tab5 or waveshare (got '${BOARD}')")
endif()
message(STATUS "Board: ${BOARD}")
file(WRITE "${CMAKE_SOURCE_DIR}/board_config.cmake" "set(BOARD \"${BOARD}\")\n")

# The board's component manifest becomes main/idf_component.yml (generated, untracked) before
# the component manager reads it
configure_file("${CMAKE_SOURCE_DIR}/main/idf_component.${BOARD}.yml"
               "${CMAKE_SOURCE_DIR}/main/idf_component.yml" COPYONLY)

# The board's full configuration is committed (sdkconfig.<board>), as SimplrSSH does: with it
# the component manager resolves once, from the lock. Shared defaults, then the board's.
# When changing a default, rebuild and commit the board's sdkconfig too.
set(SDKCONFIG "${CMAKE_SOURCE_DIR}/sdkconfig.${BOARD}")
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.${BOARD}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Each board's resolved components are locked (and committed) separately
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${BOARD}")
project(WebRadio)

# Release images, after `idf.py build`:
#   idf.py merge-bin -o WebRadio-<Board>-v<ver>-release.bin   bootloader, partition table and
#                        app in one file, flashed at 0x0 (web flasher or esptool)
#   idf.py launcher-bin  <build>/WebRadio-<Board>-v<ver>-launcher.bin: for launchers such as
#                        bmorcelli/Launcher (M5 Launcher), which install the app into their own
#                        partition table; partitions_launcher.csv adds the "webradio" settings
#                        partition the launcher creates and keeps across updates
idf_build_get_property(python PYTHON)
idf_build_get_property(idf_path IDF_PATH)
idf_build_get_property(build_dir BUILD_DIR)
if(BOARD STREQUAL "tab5")
    set(IMAGE_PREFIX "WebRadio-Tab5")
else()
    set(IMAGE_PREFIX "WebRadio-Waveshare")
endif()
set(LAUNCHER_TABLE "${build_dir}/partition_table/partition-table-launcher.bin")
set(LAUNCHER_APP_OFFSET 0x50000)   # The app's offset in partitions_launcher.csv
add_custom_target(launcher-bin
    COMMAND ${python} ${idf_path}/components/partition_table/gen_esp32part.py -q
            --flash-size ${CONFIG_ESPTOOLPY_FLASHSIZE} --offset ${CONFIG_PARTITION_TABLE_OFFSET}
            ${CMAKE_SOURCE_DIR}/partitions_launcher.csv ${LAUNCHER_TABLE}
    COMMAND ${python} -m esptool --chip ${IDF_TARGET} merge_bin
            -o ${build_dir}/${IMAGE_PREFIX}-v${PROJECT_VER}-launcher.bin
            --flash_mode ${CONFIG_ESPTOOLPY_FLASHMODE} --flash_freq ${CONFIG_ESPTOOLPY_FLASHFREQ}
            --flash_size ${CONFIG_ESPTOOLPY_FLASHSIZE}
            ${CONFIG_BOOTLOADER_OFFSET_IN_FLASH} ${build_dir}/bootloader/bootloader.bin
            ${CONFIG_PARTITION_TABLE_OFFSET} ${LAUNCHER_TABLE}
            ${LAUNCHER_APP_OFFSET} ${build_dir}/${PROJECT_NAME}.bin
    VERBATIM)
add_dependencies(launcher-bin app bootloader)
