# Host unit tests: shared protocol, firmware drive logic, PC controller mapping.
#   cmake -S tests -B tests/build && cmake --build tests/build && tests/build/fpv-tests
cmake_minimum_required(VERSION 3.20)
project(fpv_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(fpv-tests test_main.cpp)
target_include_directories(fpv-tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/esp32/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../pc/src)
if(MSVC)
  target_compile_options(fpv-tests PRIVATE /W4)
else()
  target_compile_options(fpv-tests PRIVATE -Wall -Wextra)
endif()
if(MINGW)
  target_link_options(fpv-tests PRIVATE -static)
endif()
enable_testing()
add_test(NAME fpv-tests COMMAND fpv-tests)
