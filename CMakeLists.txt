cmake_minimum_required(VERSION 3.21)
project(EyeTrackVR-Overlay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies (provided by vcpkg via the toolchain file; see README).
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
find_package(Vulkan REQUIRED)
find_package(OpenXR CONFIG REQUIRED)

# stb is header-only; locate its include directory on the vcpkg prefix.
find_path(STB_INCLUDE_DIR NAMES stb_image.h REQUIRED)

file(GLOB SOURCES "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/src/*.h")

add_executable(EyeTrackVR-Overlay ${SOURCES})

target_include_directories(EyeTrackVR-Overlay PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${STB_INCLUDE_DIR}
)

target_link_libraries(EyeTrackVR-Overlay PRIVATE
    OpenXR::openxr_loader
    Vulkan::Vulkan
)

# OpenVR overlay backend (src/ovr_app.cpp). Enabled on Windows so the SteamVR
# path can composite over a running game instead of evicting it. Uses the
# bundled SDK in external/OpenVR. When disabled, ovr_app.cpp compiles to inert
# stubs (HAVE_OPENVR undefined) and only the OpenXR backend is available.
set(OPENVR_SDK_DIR "${PROJECT_SOURCE_DIR}/external/OpenVR")
if(WIN32 AND EXISTS "${OPENVR_SDK_DIR}/headers/openvr.h")
    target_compile_definitions(EyeTrackVR-Overlay PRIVATE HAVE_OPENVR)
    target_include_directories(EyeTrackVR-Overlay PRIVATE "${OPENVR_SDK_DIR}/headers")
    target_link_libraries(EyeTrackVR-Overlay PRIVATE
        "${OPENVR_SDK_DIR}/lib/win64/openvr_api.lib")
    add_custom_command(TARGET EyeTrackVR-Overlay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENVR_SDK_DIR}/bin/win64/openvr_api.dll" $<TARGET_FILE_DIR:EyeTrackVR-Overlay>
    )
endif()

if(WIN32)
    # net.cpp uses Winsock; OpenXR/Vulkan loaders ship as DLLs.
    target_link_libraries(EyeTrackVR-Overlay PRIVATE ws2_32)
    add_custom_command(TARGET EyeTrackVR-Overlay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:EyeTrackVR-Overlay> $<TARGET_FILE_DIR:EyeTrackVR-Overlay>
        COMMAND_EXPAND_LISTS
    )
endif()

# Assets next to the binary: the dot image and the bundled font.
add_custom_command(TARGET EyeTrackVR-Overlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/src/assets" $<TARGET_FILE_DIR:EyeTrackVR-Overlay>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/external/font/OpenSans-Regular.ttf"
        $<TARGET_FILE_DIR:EyeTrackVR-Overlay>/assets/OpenSans-Regular.ttf
)
