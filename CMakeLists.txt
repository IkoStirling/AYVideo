cmake_minimum_required(VERSION 3.20)
project(AYVideo VERSION 3.0.0 LANGUAGES CXX)
set(MODULE_NAME AYVideo)
if(NOT TARGET AYAudioPlayback)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../AYAudio/playback" "${CMAKE_CURRENT_BINARY_DIR}/audio-playback")
endif()
add_subdirectory(playback)
add_library(AYVideo STATIC src/AYVideoSubSystem.cpp src/AYVideoRuntimeModule.cpp)
target_include_directories(AYVideo PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ecs")
target_link_libraries(AYVideo PUBLIC AYVideoPlayback AYAudio AYGameLoop AY::Module)
set(AYVIDEO_PUBLIC_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(AYVIDEO_INTERFACE_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/interface")

add_custom_target(ayvideo_check_no_ffmpeg_in_public_headers
    COMMAND ${CMAKE_COMMAND}
        -DAYTGT_PUBLIC_HEADER_DIR="${AYVIDEO_PUBLIC_HEADER_DIR}"
        -DAYTGT_MODULE_NAME="${MODULE_NAME}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoFFmpegInPublicHeaders.cmake"
    COMMAND ${CMAKE_COMMAND}
        -DAYTGT_PUBLIC_HEADER_DIR="${AYVIDEO_INTERFACE_HEADER_DIR}"
        -DAYTGT_MODULE_NAME="${MODULE_NAME}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoFFmpegInPublicHeaders.cmake"
    COMMENT "AYVideo public-header ffmpeg-family leak guard (design.md §4.3)"
    VERBATIM
)


add_dependencies(AYVideo ayvideo_check_no_ffmpeg_in_public_headers)
if(BUILD_TESTING)
    add_subdirectory(unittest)
endif()
if(TARGET AYRenderer)
    add_subdirectory(demo)
endif()
