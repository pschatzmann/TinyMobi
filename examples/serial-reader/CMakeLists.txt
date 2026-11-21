cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(serial-reader)
endif()

# Builds the sketch as a program for the desktop with the Arduino emulator.
# The emulator's SD card uses host paths, so the book and the output
# directory for the cover are given here:
#   cmake -S examples/serial-reader -B build-serial-reader -DTINYMOBI_EXAMPLE_BOOK=/path/book.mobi
#   cmake --build build-serial-reader
#   ./build-serial-reader/serial-reader
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Emulator.cmake)

set(TINYMOBI_EXAMPLE_BOOK "/book.mobi" CACHE FILEPATH "Book to read")
set(TINYMOBI_EXAMPLE_COVER "cover" CACHE STRING "Cover file name without extension (written next to the program)")

arduino_sketch(serial-reader serial-reader.ino LIBRARIES TinyMobi
  DEFINITIONS
    TINYMOBI_BOOK="${TINYMOBI_EXAMPLE_BOOK}"
    TINYMOBI_COVER="${TINYMOBI_EXAMPLE_COVER}")
# The Arduino IDE adds Arduino.h to each .ino file
target_compile_options(serial-reader PRIVATE -include Arduino.h)
