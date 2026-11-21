cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(gui-reader)
endif()

# Builds the reader GUI as a desktop program: the Arduino emulator and
# TinyMaterialDesign (with TinyGPU) are fetched from GitHub. For local
# checkouts pass -DFETCHCONTENT_SOURCE_DIR_<NAME>=<path>.
#   cmake -S examples/gui-reader -B build-gui-reader -DTINYMOBI_EXAMPLE_BOOK=/path/book.mobi
#   cmake --build build-gui-reader
#   ./build-gui-reader/gui-reader
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

# A book opened at startup (optional: without one the open dialog is shown) and
# the folder of the open dialog (default: the folder of the book, else the root).
set(TINYMOBI_EXAMPLE_BOOK "" CACHE FILEPATH "Book to open at startup (optional)")
if(TINYMOBI_EXAMPLE_BOOK)
  get_filename_component(TINYMOBI_EXAMPLE_LIBRARY_DEFAULT "${TINYMOBI_EXAMPLE_BOOK}" DIRECTORY)
else()
  set(TINYMOBI_EXAMPLE_LIBRARY_DEFAULT "/")
endif()
set(TINYMOBI_EXAMPLE_LIBRARY "${TINYMOBI_EXAMPLE_LIBRARY_DEFAULT}" CACHE PATH "Folder of the open dialog")
arduino_sketch(gui-reader gui-reader.ino LIBRARIES TinyMobi TinyMaterialDesign TinyJPEG TinyFonts SDL2::SDL2
  DEFINITIONS TINYMOBI_BOOK="${TINYMOBI_EXAMPLE_BOOK}" TINYMOBI_LIBRARY="${TINYMOBI_EXAMPLE_LIBRARY}")
target_compile_options(gui-reader PRIVATE -include Arduino.h)
target_compile_definitions(gui-reader PRIVATE TINYGPU_AUTO_NAMESPACE TINYMD_AUTO_NAMESPACE ARDUINO IS_DESKTOP)
# the emulated SD card lists directories with std::filesystem (C++17)
target_compile_features(gui-reader PRIVATE cxx_std_17)
target_compile_definitions(gui-reader PRIVATE USE_FILESYSTEM)
