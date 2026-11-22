# TinyMobi

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue.svg)](https://www.arduino.cc/reference/en/libraries/)
[![CMake](https://img.shields.io/badge/CMake-Supported-blue.svg)](https://github.com/pschatzmann/TinyMobi/blob/main/CMakeLists.txt)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](docs/license.md)

## Introduction

TinyMobi is a header-only MOBI / PalmDOC parser for Arduino and desktop C++.
It is designed for document readers on microcontrollers: the book is streamed
from memory or from an SD card, one record at a time, into a fixed buffer.

## Features

- Header only: `#include <TinyMobi.h>`, no heap allocation.
- The file is never loaded completely. Headers, metadata and the record table
  are read on demand, and text is decompressed one record at a time into a
  fixed buffer (`RecordBufferSize`, default 4096 bytes).
- Works with memory buffers (`MemorySource`) and any seekable file
  (`StreamSource<File>`: SD, LittleFS, FFat, SPIFFS, SdFat).
- Text compression: PalmDOC (LZ77), HUFF/CDIC and uncompressed, with trailing
  entries and multibyte overlap.
- UTF-8 output; CP1252 books are converted on the fly.
- Markup mode (raw HTML-like book text) or plain text mode (tags removed, block
  tags become newlines, entities decoded).
- One text stream for every format: `read()` delivers the book in reading order
  (KF8 parts are assembled automatically). `position()` / `seek()` give
  section-based bookmarks.
- Metadata: title, author, publisher, language, date, cover record.
- Table of contents (NCX index): titles, start positions, levels and the tree
  (not the start positions of KF8 books, see the limitations). The links of the
  text (`filepos` and `kindle:pos`) resolve to positions with `resolveLink()`, also
  in KF8 books.
- Raw access to any PalmDB record with `readRecord()` and `recordLength()`.
  `imageType()` recognizes JPEG, PNG, GIF and BMP data, so covers can be saved.
- Image references in the text (`<img recindex>` and `kindle:embed`).
- Optional reader GUI for TinyMaterialDesign and TinyFonts (`src/TinyMobi/MobiReaderApp.h`):
  page-by-page reading with swipe navigation, tappable links (the table of
  contents is part of the text), headings, a title bar with the page number, a
  system menu with a settings screen (dark mode, navigation buttons, page number),
  and the JPEG cover on the first page.
- KF8 (AZW3 and hybrid MOBI): the XHTML parts are assembled from the skeleton
  and fragment tables and read like any other text. The output is byte identical
  to KindleUnpack on the test books.

Limitations (DRM, images are not decoded, and others) are listed in
[docs/limitations.md](docs/limitations.md).

## Documentation

- [Usage](docs/usage.md): opening books, reading text, bookmarks, the table of
  contents, images, the cover, the no-KF8 build, and the Arduino example
- [Desktop build and tests](docs/desktop.md): CMake, the test suite, and the
  real books used for testing
- [Reader GUI](docs/gui.md): the TinyMaterialDesign reader class and its desktop
  example
- [Running the example on the desktop](docs/emulator.md): the Arduino emulator
- [Limitations](docs/limitations.md): what is not supported
- [Layout](docs/layout.md): the files of the project
- [API reference](https://pschatzmann.github.io/TinyMobi/namespaces.html) Doxygen Class Documentation
- [License](docs/license.md)

## Installation in Arduino

You can download the library as zip and call include Library -> zip library. Or you can git clone this project into the Arduino libraries folder e.g. with

```
cd  ~/Documents/Arduino/libraries
git clone https://github.com/pschatzmann/TinyMobi.git
```

I recommend to use git because you can easily update to the latest version just by executing the ```git pull``` command in the project folder.

Then add `#include <TinyMobi.h>` to your sketch. See the [usage guide](docs/usage.md) and the `serial-reader` example.

For desktop builds with CMake, see [Desktop build and tests](docs/desktop.md).
