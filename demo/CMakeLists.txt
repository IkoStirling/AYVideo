# AYVideo/demo/CMakeLists.txt — V3 engine demo (design.md §12 / §17).
#
# Mirrors AY2D/demo shaderc + bgfx hint wiring. Acceptance:
#   AYVIDEO_DEMO_FRAMES=70 AYVIDEO_DEMO_PATH=1|2  AYVideo_EngineDemo
#   powershell -File check_screenshot.ps1 <shots>/frame_30.tga ...

set(AY_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../AYShader")

list(APPEND CMAKE_PREFIX_PATH
    "${AY_SHADER_ROOT}/thirdParty/bgfx-install/debug"
    "${AY_SHADER_ROOT}/thirdParty/bgfx-install/release"
)
find_package(bgfx CONFIG REQUIRED)

if(NOT DEFINED AY_SHADER_SHADERC_PATH)
    if(WIN32)
        set(_ays_shaderc_name "shaderc.exe")
    else()
        set(_ays_shaderc_name "shaderc")
    endif()
    set(AY_SHADER_SHADERC_PATH
        "${AY_SHADER_ROOT}/thirdParty/bgfx-install/debug/bin/${_ays_shaderc_name}")
endif()

if(NOT DEFINED AY_BGFX_SHADER_INCLUDE_DIR)
    set(AY_BGFX_SHADER_INCLUDE_DIR
        "${AY_SHADER_ROOT}/thirdParty/bgfx-install/debug/include/bgfx")
endif()

get_filename_component(AY_SHADER_SHADERC_PATH "${AY_SHADER_SHADERC_PATH}" ABSOLUTE)
get_filename_component(AY_BGFX_SHADER_INCLUDE_DIR "${AY_BGFX_SHADER_INCLUDE_DIR}" ABSOLUTE)

if(NOT DEFINED AY_SHADER_BGFX_COMMON_DIR)
    set(_ays_bgfx_search
        "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/bgfx/examples/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../thirdparty/bgfx/examples/common"
    )
    set(AY_SHADER_BGFX_COMMON_DIR "")
    foreach(_dir IN LISTS _ays_bgfx_search)
        if(EXISTS "${_dir}/common.sh")
            set(AY_SHADER_BGFX_COMMON_DIR "${_dir}")
            break()
        endif()
    endforeach()
endif()
if(NOT DEFINED AY_SHADER_BGFX_SRC_DIR AND AY_SHADER_BGFX_COMMON_DIR)
    set(AY_SHADER_BGFX_SRC_DIR "${AY_SHADER_BGFX_COMMON_DIR}/../../src")
endif()

add_executable(AYVideo_EngineDemo AYVideoEngineDemo.cpp)

target_link_libraries(AYVideo_EngineDemo PRIVATE
    AYVideo
    AYRenderer
    AYGameLoop
)

# Reach backend/RendererVideoFrameTexture.h for gpuHandle().
target_include_directories(AYVideo_EngineDemo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(WIN32)
    target_link_libraries(AYVideo_EngineDemo PRIVATE user32 gdi32)
endif()

target_compile_definitions(AYVideo_EngineDemo PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
)

if(AYVIDEO_HAS_AYRENDERER)
    target_compile_definitions(AYVideo_EngineDemo PRIVATE AYVIDEO_HAS_AYRENDERER=1)
endif()
if(AYVIDEO_HAS_FFMPEG)
    target_compile_definitions(AYVideo_EngineDemo PRIVATE AYVIDEO_HAS_FFMPEG=1)
endif()
