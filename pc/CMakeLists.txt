cmake_minimum_required(VERSION 3.20)
project(fpv_pc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Prebuilt dependencies from fetch_deps.ps1 (override with -DSDL3_DIR / -DFFMPEG_DIR).
set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
list(APPEND CMAKE_PREFIX_PATH ${THIRD_PARTY}/SDL3)
find_package(SDL3 REQUIRED CONFIG)

set(FFMPEG_DIR ${THIRD_PARTY}/ffmpeg CACHE PATH "FFmpeg shared build (include/, lib/, bin/)")
if(NOT EXISTS ${FFMPEG_DIR}/include/libavcodec/avcodec.h)
  message(FATAL_ERROR "FFmpeg not found in ${FFMPEG_DIR}. Run fetch_deps.ps1 first.")
endif()
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE ${FFMPEG_DIR}/include)
if(MSVC)
  target_link_libraries(ffmpeg INTERFACE ${FFMPEG_DIR}/lib/avcodec.lib ${FFMPEG_DIR}/lib/avutil.lib)
else()
  target_link_libraries(ffmpeg INTERFACE ${FFMPEG_DIR}/lib/libavcodec.dll.a ${FFMPEG_DIR}/lib/libavutil.dll.a)
endif()

add_executable(fpv-pc
  src/main.cpp
  src/config.cpp
  src/gamepad.cpp
  src/udp_link.cpp
  src/serial_link.cpp
  src/video.cpp
  src/osd.cpp
)
target_include_directories(fpv-pc PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_compile_definitions(fpv-pc PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(fpv-pc PRIVATE SDL3::SDL3 ffmpeg ws2_32 winmm)
if(MINGW)
  target_compile_options(fpv-pc PRIVATE -Wall -Wextra -Wno-unused-parameter)
  # No MinGW runtime DLLs to ship.
  target_link_options(fpv-pc PRIVATE -static)
endif()

# Put the runtime DLLs and an example config next to the exe.
file(GLOB FFMPEG_DLLS ${FFMPEG_DIR}/bin/avcodec-*.dll ${FFMPEG_DIR}/bin/avutil-*.dll ${FFMPEG_DIR}/bin/swresample-*.dll)
add_custom_command(TARGET fpv-pc POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> ${FFMPEG_DLLS} $<TARGET_FILE_DIR:fpv-pc>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/fpv.ini.example $<TARGET_FILE_DIR:fpv-pc>
)

# Stand-in phone for testing the PC side without the truck (tools/fake_phone.cpp).
add_executable(fpv-fake-phone ../tools/fake_phone.cpp)
target_include_directories(fpv-fake-phone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_compile_definitions(fpv-fake-phone PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(fpv-fake-phone PRIVATE ffmpeg ws2_32)
if(MINGW)
  target_link_options(fpv-fake-phone PRIVATE -static)
endif()
