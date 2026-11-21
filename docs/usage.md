# Usage

[Back to the README](../README.md)

## Opening a book

```cpp
#include <TinyMobi.h>

// memory buffer
tinymobi::MemoryMobi doc(tinymobi::MemorySource(data, len));
if (doc.open() == tinymobi::Error::None) {
  uint8_t buf[256];
  size_t n = doc.read(buf, sizeof(buf));   // UTF-8 text, 0 at the end
}

// SD card file (keep the file open while using the document)
File f = SD.open("/book.mobi");
tinymobi::FileMobi<File> doc(tinymobi::StreamSource<File>(f), tinymobi::TextMode::Plain);
```

## Reading the text

`read()` delivers the whole book in reading order. KF8 books (AZW3 and hybrid
MOBI) are read as their assembled KF8 text, and the parts follow each other
automatically. No format-specific calls are needed.

- `TextMode::Markup` (the default) returns the book's HTML-like markup.
- `TextMode::Plain` removes the tags, turns block tags into newlines and
  decodes the entities. Use `setTextMode()` to change the mode.
- `TextMode::PlainLinks` is `Plain` with markers: each link is wrapped in
  `\x01target\x02text\x03` and each heading in `\x04...\x05`. Pass a target to
  `resolveLink()` to get its position. Headings are wrapped in `\x04` and `\x05` on
  their own lines.

## Bookmarks

`position()` returns a `Position` (a section and an offset in it), and
`seek()` returns to it. `rewind()` goes back to the start of the book.

```cpp
tinymobi::Position bookmark = doc.position();
// ... read on ...
doc.seek(bookmark);
```

Seeking into the middle of a KF8 part rebuilds that part up to the offset.

## Table of contents

```cpp
doc.forEachTocEntry([](const tinymobi::TocEntry& e) -> bool {
  // e.title, e.level, e.start (where the section starts) and the tree
  // (e.parent, e.firstChild, e.lastChild) are available here
  return true;  // return false to stop
});
```

`tocCount()` returns the number of entries, and `tocEntry(i, entry)` returns
one entry. `e.start` can be passed to `seek()`. For KF8 books `e.start` is
unknown (`section == kNoRecord`), see [Limitations](limitations.md).

The table of contents is also part of the text: in `TextMode::PlainLinks` its
entries are links, and `resolveLink()` gives the position of each link, MOBI6
(`filepos`) and KF8 (`kindle:pos`).

## Images

`forEachImageRef()` calls a function for every image tag of the book, in
reading order. Each `ImageRef` gives the `Position` of the tag and the PalmDB
record of the image. The reading position is kept.

```cpp
doc.forEachImageRef([&](const tinymobi::ImageRef& ref) -> bool {
  if (ref.record != tinymobi::kNoRecord) {
    // read the image with readRecord(ref.record, ...)
  }
  return true;  // return false to stop
});
```

## Saving the cover

```cpp
uint32_t cover = doc.info().coverRecord;   // tinymobi::kNoRecord if none
uint32_t len = 0;
if (doc.recordLength(cover, len) == tinymobi::Error::None) {
  uint8_t* data = new uint8_t[len];        // use ps_malloc on PSRAM boards
  size_t n = 0;
  doc.readRecord(cover, data, len, n);
  // map tinymobi::imageType(data, n) to ".jpg", ".png", ".gif" or ".bmp"
  // and write data to the SD card with that extension
  delete[] data;
}
```

## Without KF8

Microcontrollers that do not need KF8 can use `MemoryMobiNoKf8` (or
`MobiDocument<Source, 4096, false>`). It drops the 4 KB KF8 source buffer. The
book is then read as its MOBI6 text.

## Arduino example

`examples/serial-reader` is a complete sketch. It prints the metadata and the
opening text, bookmarks the reading position, and saves the cover to the SD
card. It can also be run on the desktop, see
[Running the example on the desktop](emulator.md).
