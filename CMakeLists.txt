cmake_minimum_required(VERSION 3.16)
project(cpz-baby-monitor VERSION 0.0.1 LANGUAGES C CXX ASM)

# Version stamped into the binary, the .deb and `--version`. CI overrides it
# from the release tag (v1.2.3 -> 1.2.3) so a release and its package agree.
set(CPZBM_VERSION "${PROJECT_VERSION}" CACHE STRING "Application/package version")

# CPZBM_DEVICE=ON  : full app for the CardputerZero (fbdev, SX1262, audio)
# CPZBM_DEVICE=OFF : hardware-free core library + unit tests (host)
option(CPZBM_DEVICE "Build the CardputerZero device application" ON)
option(BUILD_TESTING "Build unit tests" OFF)
set(DEPS "${CMAKE_CURRENT_LIST_DIR}/dependencies")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
include(CTest)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(CODEC2 REQUIRED IMPORTED_TARGET codec2)

set(WARN -Wall -Wextra -Wpedantic)

# --- hardware-free core: wire format, crypto, link, store, audio DSP + codec ---
add_library(cpzbm_core STATIC
    src/proto/wire.cpp
    src/proto/crypto.cpp
    src/proto/link.cpp
    src/store/store.cpp
    src/audio/codec.cpp
)
target_include_directories(cpzbm_core PUBLIC src)
target_compile_options(cpzbm_core PRIVATE ${WARN})
target_link_libraries(cpzbm_core PUBLIC PkgConfig::SODIUM PkgConfig::CODEC2)

if(BUILD_TESTING)
    foreach(t wire crypto link audio region)
        add_executable(${t}_test tests/${t}_test.cpp)
        target_include_directories(${t}_test PRIVATE tests)
        target_compile_options(${t}_test PRIVATE ${WARN})
        target_link_libraries(${t}_test PRIVATE cpzbm_core m)
        add_test(NAME ${t} COMMAND ${t}_test)
    endforeach()
endif()

if(NOT CPZBM_DEVICE)
    return()
endif()

# --- LVGL (fbdev) and spdlog ---
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/lvgl" "${CMAKE_CURRENT_BINARY_DIR}/lvgl" EXCLUDE_FROM_ALL)
add_subdirectory("${DEPS}/spdlog" "${CMAKE_CURRENT_BINARY_DIR}/spdlog" EXCLUDE_FROM_ALL)
target_compile_definitions(lvgl PUBLIC CAP_LORA_LVGL_USE_SDL=0 CAP_LORA_LVGL_FS_POSIX_PATH="/")

# --- RadioLib SX1262 subset (same as upstream Cap-LoRa-1262) ---
set(RL "${DEPS}/RadioLib/src")
add_library(cpzbm_radiolib STATIC
    ${RL}/Hal.cpp ${RL}/Module.cpp ${RL}/utils/CRC.cpp ${RL}/utils/ConvCode.cpp ${RL}/utils/Utils.cpp
    ${RL}/protocols/PhysicalLayer/PhysicalLayer.cpp
    ${RL}/modules/SX126x/SX126x.cpp ${RL}/modules/SX126x/SX126x_commands.cpp
    ${RL}/modules/SX126x/SX126x_config.cpp ${RL}/modules/SX126x/SX126x_LR_FHSS.cpp
    ${RL}/modules/SX126x/SX1262.cpp)
target_include_directories(cpzbm_radiolib SYSTEM PUBLIC ${RL})

# --- Pigweed Linux SPI/I2C/GPIO subset (same as upstream; C++20) ---
set(PW "${DEPS}/pigweed")
add_library(cpzbm_pigweed STATIC
    ${PW}/pw_spi_linux/spi.cc ${PW}/pw_digital_io/digital_io.cc ${PW}/pw_digital_io_linux/digital_io.cc
    ${PW}/pw_digital_io_linux/notifier.cc ${PW}/pw_i2c_linux/initiator.cc ${PW}/pw_i2c/initiator.cc
    ${PW}/pw_i2c/address.cc ${PW}/pw_chrono/system_clock.cc ${PW}/pw_sync/mutex.cc ${PW}/pw_sync/timed_mutex.cc
    ${PW}/pw_sync_stl/mutex.cc ${PW}/pw_assert_trap/trap_handler.cc ${PW}/pw_string/format.cc
    ${PW}/pw_string/string_builder.cc ${PW}/pw_string/type_to_string.cc ${PW}/pw_status/status.cc)
set(PW_INC)
foreach(m pw_spi_linux pw_spi pw_bytes pw_span pw_digital_io_linux pw_i2c_linux pw_digital_io pw_i2c pw_status
          pw_result pw_function pw_thread pw_memory pw_chrono pw_sync pw_containers pw_numeric pw_string
          pw_preprocessor pw_polyfill pw_toolchain pw_log pw_log_null pw_assert pw_assert_trap pw_chrono_stl
          pw_sync_stl)
    list(APPEND PW_INC "${PW}/${m}/public")
endforeach()
list(APPEND PW_INC
    "${PW}/pw_log_null/public_overrides"
    "${PW}/pw_assert_trap/assert_public_overrides"
    "${PW}/pw_assert_trap/check_public_overrides"
    "${PW}/pw_chrono_stl/clock_public_overrides"
    "${PW}/pw_sync_stl/public_overrides"
    "${PW}/third_party/fuchsia/repo/sdk/lib/stdcompat/include"
    "${PW}/third_party/fuchsia/repo/sdk/lib/fit/include")
target_include_directories(cpzbm_pigweed SYSTEM PUBLIC ${PW_INC})
target_compile_options(cpzbm_pigweed PRIVATE -Wno-return-type)
set_target_properties(cpzbm_pigweed PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(cpzbm_pigweed PUBLIC Threads::Threads)

# --- application ---
add_executable(cpz-baby-monitor
    src/main.cpp
    src/app/app.cpp
    src/app/debug_modes.cpp
    src/ui/ui.cpp
    src/ui/fonts.cpp
    src/hal/gps_lvgl_hal.cpp
    src/input/gps_keypad.cpp
    src/radio/radio.cpp
    src/radio/hw/cap_lora_1262.cpp
    src/radio/hw/driver/PI4IO/pi4ioe5v6408_driver.cpp
    src/audio/audio_io.cpp
)
# The SX1262 facade shares pw::span with the C++20 pigweed library.
set_target_properties(cpz-baby-monitor PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(cpz-baby-monitor PRIVATE src "${DEPS}/miniaudio")
target_compile_definitions(cpz-baby-monitor PRIVATE CPZBM_VERSION="${CPZBM_VERSION}")
target_compile_options(cpz-baby-monitor PRIVATE ${WARN})
# miniaudio is a single-header library compiled into audio_io.cpp.
set_source_files_properties(src/audio/audio_io.cpp PROPERTIES COMPILE_OPTIONS "-Wno-pedantic;-Wno-unused-parameter;-Wno-missing-field-initializers")
target_link_libraries(cpz-baby-monitor PRIVATE
    cpzbm_core cpzbm_radiolib cpzbm_pigweed lvgl::lvgl spdlog::spdlog Threads::Threads ${CMAKE_DL_LIBS} m)
