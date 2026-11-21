cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(gui-screenshots)
endif()

# Builds the screenshot tool for the reader GUI as a desktop program: the Arduino emulator and
# TinyMaterialDesign (with TinyGPU) are fetched from GitHub. For local
# checkouts pass -DFETCHCONTENT_SOURCE_DIR_<NAME>=<path>.
#   cmake -S tools/gui-screenshots -B build-gui-screenshots -DTINYMOBI_EXAMPLE_BOOK=/path/book.mobi
#   cmake --build build-gui-screenshots
#   TINYMOBI_SCREENSHOTS=<dir> ./build-gui-screenshots/gui-screenshots
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Emulator.cmake)

include(FetchContent)
# we want only the library, not its examples and tests
set(TMD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TMD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TGPU_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  tinymaterialdesign
  GIT_REPOSITORY https://github.com/pschatzmann/TinyMaterialDesign.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(tinymaterialdesign)

# TinyJPEG decodes the cover
set(TINYJPEG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  tinyjpeg
  GIT_REPOSITORY https://github.com/pschatzmann/TinyJPEG.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(tinyjpeg)

# TinyFonts draws the text (Latin-1 characters with the Lato fonts)
set(TINYFONTS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TINYFONTS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  tinyfonts
  GIT_REPOSITORY https://github.com/pschatzmann/TinyFonts.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(tinyfonts)

find_package(SDL2 REQUIRED)

set(TINYMOBI_EXAMPLE_BOOK "/book.mobi" CACHE FILEPATH "Book to read")
arduino_sketch(gui-screenshots gui-screenshots.ino LIBRARIES TinyMobi TinyMaterialDesign TinyJPEG TinyFonts SDL2::SDL2
  DEFINITIONS TINYMOBI_BOOK="${TINYMOBI_EXAMPLE_BOOK}")
target_compile_options(gui-screenshots PRIVATE -include Arduino.h)
target_compile_definitions(gui-screenshots PRIVATE TINYGPU_AUTO_NAMESPACE TINYMD_AUTO_NAMESPACE ARDUINO IS_DESKTOP)
