cmake_minimum_required(VERSION 3.16)

project(TinyMobi VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Header-only library target
# ---------------------------------------------------------------------------
add_library(TinyMobi INTERFACE)
add_library(TinyMobi::TinyMobi ALIAS TinyMobi)
target_include_directories(TinyMobi
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>)
target_compile_features(TinyMobi INTERFACE cxx_std_11)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# Tests are only built when TinyMobi is the top level project (or on request),
# so consumers that add_subdirectory() the library do not pull them in.
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  set(_tinymobi_default_tests ON)
else()
  set(_tinymobi_default_tests OFF)
endif()
option(TINYMOBI_BUILD_TESTS "Build and register the desktop tests" ${_tinymobi_default_tests})
option(TINYMOBI_SANITIZE "Build the tests with AddressSanitizer and UBSan" OFF)
set(TINYMOBI_SAMPLE_DIR "" CACHE PATH
    "Optional directory with real .mobi/.azw files; each one is parsed and streamed by the tests")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# The desktop examples (serial-reader, gui-reader) are built with the Arduino
# emulator, which is fetched from GitHub on the first configure.
option(TINYMOBI_BUILD_EXAMPLES "Build the desktop examples" ON)
if(TINYMOBI_BUILD_EXAMPLES)
  add_subdirectory(examples)
  find_package(SDL2 QUIET)
  if(SDL2_FOUND)
    add_subdirectory(tools/gui-screenshots)
  endif()
endif()

if(TINYMOBI_BUILD_TESTS)
  include(CTest)
  if(BUILD_TESTING)
    add_executable(tinymobi_tests tests/test_tinymobi.cpp)
    target_link_libraries(tinymobi_tests PRIVATE TinyMobi::TinyMobi)

    if(MSVC)
      target_compile_options(tinymobi_tests PRIVATE /W4)
    else()
      target_compile_options(tinymobi_tests PRIVATE -Wall -Wextra -pedantic)
    endif()

    if(TINYMOBI_SANITIZE AND NOT MSVC)
      target_compile_options(tinymobi_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(tinymobi_tests PRIVATE -fsanitize=address,undefined)
    endif()

    # Synthetic MOBI files built in memory: always run, no external data needed
    add_test(NAME tinymobi.synthetic COMMAND tinymobi_tests)
    set_tests_properties(tinymobi.synthetic PROPERTIES LABELS "unit")

    # The GUI: the screenshot tool runs headless (SDL dummy video driver) and must
    # write the cover, a text page and the settings screen. It needs a book.
    if(TARGET gui-screenshots AND TINYMOBI_EXAMPLE_BOOK AND EXISTS "${TINYMOBI_EXAMPLE_BOOK}")
      set(TINYMOBI_GUI_SHOTS_DIR "${CMAKE_BINARY_DIR}/gui-screenshots")
      add_test(NAME gui.screenshots
        COMMAND sh -c "rm -rf '${TINYMOBI_GUI_SHOTS_DIR}' && mkdir -p '${TINYMOBI_GUI_SHOTS_DIR}' && \
          TINYMOBI_SCREENSHOTS='${TINYMOBI_GUI_SHOTS_DIR}' SDL_VIDEODRIVER=dummy $<TARGET_FILE:gui-screenshots> \
          && test -s '${TINYMOBI_GUI_SHOTS_DIR}/cover.bmp' && test -s '${TINYMOBI_GUI_SHOTS_DIR}/page1.bmp' \
          && test -s '${TINYMOBI_GUI_SHOTS_DIR}/settings.bmp'")
      set_tests_properties(gui.screenshots PROPERTIES LABELS "gui")
    endif()

    # Real books (optional)
    if(TINYMOBI_SAMPLE_DIR)
      file(GLOB_RECURSE TINYMOBI_SAMPLE_FILES CONFIGURE_DEPENDS
           "${TINYMOBI_SAMPLE_DIR}/*.mobi" "${TINYMOBI_SAMPLE_DIR}/*.azw"
           "${TINYMOBI_SAMPLE_DIR}/*.azw3")
      if(TINYMOBI_SAMPLE_FILES)
        add_test(NAME tinymobi.samples COMMAND tinymobi_tests ${TINYMOBI_SAMPLE_FILES})
        set_tests_properties(tinymobi.samples PROPERTIES LABELS "samples")
      else()
        message(WARNING "TINYMOBI_SAMPLE_DIR contains no .mobi/.azw files")
      endif()
    endif()
  endif()
endif()
