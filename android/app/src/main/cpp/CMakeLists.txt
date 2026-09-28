cmake_minimum_required(VERSION 3.22)
project(rover CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared wire protocol (repo root /common).
get_filename_component(FPV_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../common ABSOLUTE)

add_library(rover SHARED
  jni.cpp
  engine.cpp
  usb_serial.cpp
  video.cpp
)
target_include_directories(rover PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FPV_COMMON})
target_compile_options(rover PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
target_link_libraries(rover PRIVATE camera2ndk mediandk android log)
