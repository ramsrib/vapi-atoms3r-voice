# Vapi AtomS3R Voice — talk to a Vapi assistant from an M5Stack AtomS3R stacked
# on an Atomic Echo Base, over Vapi's `vapi.websocket` transport.
#
# The audio stack comes from Espressif's esp-webrtc-solution, vendored as a git
# submodule under deps/ and pinned to an exact commit. Clone with
# --recurse-submodules (or run `git submodule update --init`); the Makefile
# checks and tells you if it is missing.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(WEBRTC_SDK "${CMAKE_CURRENT_LIST_DIR}/deps/esp-webrtc-solution")
if(NOT EXISTS "${WEBRTC_SDK}/components")
    message(FATAL_ERROR
        "esp-webrtc-solution not found at ${WEBRTC_SDK}.\n"
        "Run:  git submodule update --init --recursive")
endif()

# Named one by one rather than adding the whole components/ directory, because
# IDF builds every component it discovers there. Pulling in the sibling
# esp_webrtc would compile the entire peer-connection stack (libsrtp, ICE, DTLS)
# into a firmware that opens no peer connection — and, more immediately, its
# manifest pins espressif/esp_websocket_client to ~1.4.0, which collides with
# the version this project asks for.
foreach(_c av_render codec_board esp_capture media_lib_sal webrtc_utils)
    list(APPEND EXTRA_COMPONENT_DIRS "${WEBRTC_SDK}/components/${_c}")
endforeach()
# network.c / sys_state.c — the WiFi and task-stats helpers main.c uses.
list(APPEND EXTRA_COMPONENT_DIRS "${WEBRTC_SDK}/solutions/common")

# --- Optional .env support --------------------------------------------------
# If a .env file exists next to this CMakeLists, a small helper turns its
# KEY=VALUE lines into main/env_config.generated.h (gitignored). settings.h
# prefers those values over the menuconfig (CONFIG_*) ones, keeping secrets out
# of sdkconfig and out of git. See .env.example for the recognized keys.
#
# A generated header (rather than -D compile definitions) is used deliberately:
# it avoids command-line quote-escaping and CMake's list-semicolon splitting, so
# API keys and WiFi passwords with special characters survive intact.
set(VAPI_ENV_FILE   "${CMAKE_CURRENT_LIST_DIR}/.env")
set(VAPI_ENV_HEADER "${CMAKE_CURRENT_LIST_DIR}/main/env_config.generated.h")
# Re-run configure (regenerating the header) whenever .env appears or changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VAPI_ENV_FILE}")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/gen_env_header.py"
                    "${VAPI_ENV_FILE}" "${VAPI_ENV_HEADER}"
    RESULT_VARIABLE _vapi_env_rc)
if(NOT _vapi_env_rc EQUAL 0)
    message(WARNING "vapi: could not generate ${VAPI_ENV_HEADER} from .env (rc=${_vapi_env_rc})")
elseif(EXISTS "${VAPI_ENV_FILE}")
    message(STATUS "vapi: applied config overrides from ${VAPI_ENV_FILE}")
endif()

project(vapi_atoms3r_voice)
