cmake_minimum_required(VERSION 3.21)
project(EyeTrackVR-Overlay VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ETVR_BUILD_TESTS "Build the unit tests (no VR hardware needed)" ON)

# Dependencies (provided by vcpkg via the toolchain file on Windows, or system
# packages on Linux; see README).
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
find_package(Vulkan REQUIRED)
find_package(OpenXR CONFIG REQUIRED)

# stb is header-only; locate its include directory on the vcpkg prefix.
find_path(STB_INCLUDE_DIR NAMES stb_image_write.h stb_truetype.h PATH_SUFFIXES stb REQUIRED)

if(MSVC)
    add_compile_options(/W3 /utf-8 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# ── Core: everything that does not touch a VR SDK ────────────────────────────
# Geometry, modes/protocol, presenter, text, sprites, networking, the session
# loop and the headless simulator. Sources are globbed (re-run the configure
# step after adding files); the VR backends and main() are listed explicitly.
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/src/*.h")
set(VR_SOURCES
    "${PROJECT_SOURCE_DIR}/src/main.cpp"
    "${PROJECT_SOURCE_DIR}/src/ovr_app.cpp"
    "${PROJECT_SOURCE_DIR}/src/ovr_app.h"
    "${PROJECT_SOURCE_DIR}/src/xr_app.cpp"
    "${PROJECT_SOURCE_DIR}/src/xr_app.h"
    "${PROJECT_SOURCE_DIR}/src/vk_base.cpp"
    "${PROJECT_SOURCE_DIR}/src/vk_base.h"
    "${PROJECT_SOURCE_DIR}/src/xr_vk.h")
list(REMOVE_ITEM CORE_SOURCES ${VR_SOURCES})

add_library(etvr_core STATIC ${CORE_SOURCES})
target_include_directories(etvr_core PUBLIC ${PROJECT_SOURCE_DIR}/src ${STB_INCLUDE_DIR})
if(WIN32)
    # net.cpp uses Winsock.
    target_link_libraries(etvr_core PUBLIC ws2_32)
endif()

# ── Executable ───────────────────────────────────────────────────────────────
add_executable(EyeTrackVR-Overlay ${VR_SOURCES})
target_link_libraries(EyeTrackVR-Overlay PRIVATE etvr_core OpenXR::openxr_loader Vulkan::Vulkan)

# OpenVR overlay backend (src/ovr_app.cpp). Enabled on Windows and Linux x86_64/aarch64
# so the SteamVR path can composite over a running game instead of evicting it.
# Uses the bundled SDK in external/OpenVR. When disabled, ovr_app.cpp compiles
# to inert stubs (HAVE_OPENVR undefined) and only the OpenXR backend is available.
set(OPENVR_SDK_DIR "${PROJECT_SOURCE_DIR}/external/OpenVR")
# The SDK's Linux library: linux64 on x86_64, linuxarm64 on aarch64 (Steam Frame).
set(OPENVR_LINUX_LIB "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        set(OPENVR_LINUX_LIB "${OPENVR_SDK_DIR}/lib/linux64/libopenvr_api.so")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(OPENVR_LINUX_LIB "${OPENVR_SDK_DIR}/lib/linuxarm64/libopenvr_api.so")
    endif()
endif()
if((WIN32 OR (OPENVR_LINUX_LIB AND EXISTS "${OPENVR_LINUX_LIB}"))
    AND EXISTS "${OPENVR_SDK_DIR}/headers/openvr.h")
    target_compile_definitions(EyeTrackVR-Overlay PRIVATE HAVE_OPENVR)
    target_include_directories(EyeTrackVR-Overlay PRIVATE "${OPENVR_SDK_DIR}/headers")
    if(WIN32)
        target_link_libraries(EyeTrackVR-Overlay PRIVATE
            "${OPENVR_SDK_DIR}/lib/win64/openvr_api.lib")
        add_custom_command(TARGET EyeTrackVR-Overlay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENVR_SDK_DIR}/bin/win64/openvr_api.dll" $<TARGET_FILE_DIR:EyeTrackVR-Overlay>
        )
    else()
        target_link_libraries(EyeTrackVR-Overlay PRIVATE "${OPENVR_LINUX_LIB}")
        set_target_properties(EyeTrackVR-Overlay PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
        add_custom_command(TARGET EyeTrackVR-Overlay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENVR_LINUX_LIB}" $<TARGET_FILE_DIR:EyeTrackVR-Overlay>
        )
    endif()
else()
    message(STATUS "OpenVR SDK not found/unsupported here: building without the overlay backend")
endif()

if(WIN32)
    # OpenXR/Vulkan loaders ship as DLLs.
    add_custom_command(TARGET EyeTrackVR-Overlay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:EyeTrackVR-Overlay> $<TARGET_FILE_DIR:EyeTrackVR-Overlay>
        COMMAND_EXPAND_LISTS
    )
endif()

# Assets next to the binary: the bundled font (the target sprites are drawn
# procedurally, so no image assets are needed any more).
add_custom_command(TARGET EyeTrackVR-Overlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:EyeTrackVR-Overlay>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/external/font/OpenSans-Regular.ttf"
        $<TARGET_FILE_DIR:EyeTrackVR-Overlay>/assets/OpenSans-Regular.ttf
)

# ── Tests ────────────────────────────────────────────────────────────────────
if(ETVR_BUILD_TESTS)
    enable_testing()
    add_executable(etvr_tests "${PROJECT_SOURCE_DIR}/tests/tests.cpp")
    target_link_libraries(etvr_tests PRIVATE etvr_core)
    target_compile_definitions(etvr_tests PRIVATE
        ETVR_FONT="${PROJECT_SOURCE_DIR}/external/font/OpenSans-Regular.ttf")
    foreach(suite i18n text geometry modes interactive session)
        add_test(NAME unit.${suite} COMMAND etvr_tests ${suite})
    endforeach()
    # The layout sweep dominates the run time; shard it so `ctest -j` uses every core.
    # Coprime with the six modes so every shard gets a mix of slow and fast ones.
    set(ETVR_LAYOUT_SHARDS 7)
    math(EXPR last_shard "${ETVR_LAYOUT_SHARDS} - 1")
    foreach(shard RANGE ${last_shard})
        add_test(NAME unit.layout.${shard} COMMAND etvr_tests layout ${shard}/${ETVR_LAYOUT_SHARDS})
    endforeach()

    # End-to-end UDP protocol test against the real executable (simulator backend).
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME protocol
                 COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tests/protocol_test.py"
                         $<TARGET_FILE:EyeTrackVR-Overlay>)
    endif()
endif()
