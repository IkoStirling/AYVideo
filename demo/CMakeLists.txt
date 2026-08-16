# AYVideo/demo/CMakeLists.txt — V3 engine demo (design.md §12 / §17).
#
# Mirrors AY2D/demo shaderc + bgfx hint wiring. Acceptance:
#   AYVIDEO_DEMO_FRAMES=70 AYVIDEO_DEMO_PATH=1|2  AYVideo_EngineDemo
#   powershell -File check_screenshot.ps1 <shots>/frame_30.tga ...

set(AY_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../AYShader")
include("${AY_SHADER_ROOT}/cmake/AYBgfx.cmake")

add_executable(AYVideo_EngineDemo AYVideoEngineDemo.cpp)

target_link_libraries(AYVideo_EngineDemo PRIVATE
    AYVideo
    AYRenderer
    AYGameLoop
    AYAudio
)

# Reach backend/RendererVideoFrameTexture.h for gpuHandle().
target_include_directories(AYVideo_EngineDemo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(WIN32)
    target_link_libraries(AYVideo_EngineDemo PRIVATE user32 gdi32 comctl32)
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
