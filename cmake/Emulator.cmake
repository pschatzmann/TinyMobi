# Fetches the Arduino emulator (https://github.com/pschatzmann/arduino-emulator)
# once and makes it available to the examples: the sketches run on the desktop.
# Include it with
#   include(<root of this library>/cmake/Emulator.cmake)
# in each CMakeLists.txt that needs it: it does its work only the first time.
#
# It provides
#   - the target arduino_emulator (the Arduino API for the desktop)
#   - the target TinyMobi (this library, header-only)
#   - the functions arduino_library() and arduino_sketch() of the emulator
# To use a local checkout of the emulator, pass
#   -DFETCHCONTENT_SOURCE_DIR_ARDUINO_EMULATOR=<path>
include_guard(GLOBAL)

if(NOT TARGET arduino_emulator)
  include(FetchContent)
  FetchContent_Declare(
      arduino_emulator
      GIT_REPOSITORY https://github.com/pschatzmann/arduino-emulator.git
      GIT_TAG        main
  )
  FetchContent_MakeAvailable(arduino_emulator)
endif()
include(${arduino_emulator_SOURCE_DIR}/Arduino.cmake)

if(NOT TARGET TinyMobi)
  arduino_library(TinyMobi ${CMAKE_CURRENT_LIST_DIR}/..)
endif()
