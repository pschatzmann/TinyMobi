cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(tinymobi-examples)
endif()

# The desktop examples, built with the Arduino emulator:
#   serial-reader: the text on the serial monitor, the cover saved to the SD card
#   gui-reader:    the touch GUI in a desktop window (needs SDL2)
add_subdirectory(serial-reader)

find_package(SDL2 QUIET)
if(SDL2_FOUND)
  add_subdirectory(gui-reader)
else()
  message(STATUS "gui-reader example skipped: SDL2 not found")
endif()
