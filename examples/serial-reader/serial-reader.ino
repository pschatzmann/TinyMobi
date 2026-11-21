// TinyMobi example: print the metadata and the first page of plain text of a
// MOBI book on an SD card, and save the cover image to the card.
#include <new>
#include <SD.h>
#include <TinyMobi.h>

// Paths can be set at build time (the desktop build does this, see CMakeLists.txt)
#ifndef TINYMOBI_BOOK
#define TINYMOBI_BOOK "/book.mobi"
#endif
#ifndef TINYMOBI_COVER
#define TINYMOBI_COVER "/cover"  // extension is added from the image type
#endif
static const char* kPath = TINYMOBI_BOOK;
static const char* kCoverPath = TINYMOBI_COVER;
static const int kSdCs = SS;  // change for your board

static const char* extensionFor(tinymobi::ImageType t) {
  switch (t) {
    case tinymobi::ImageType::Jpeg: return ".jpg";
    case tinymobi::ImageType::Png: return ".png";
    case tinymobi::ImageType::Gif: return ".gif";
    case tinymobi::ImageType::Bmp: return ".bmp";
    default: return nullptr;
  }
}

// Reads the cover record and writes it to the SD card.
static bool saveCover(tinymobi::FileMobi<File>& doc) {
  uint32_t cover = doc.info().coverRecord;
  if (cover == tinymobi::kNoRecord) {
    Serial.println("book has no cover");
    return false;
  }
  uint32_t len = 0;
  if (doc.recordLength(cover, len) != tinymobi::Error::None || len == 0) {
    Serial.println("cover record not readable");
    return false;
  }
  // the image is read in one piece, so it needs a buffer of its size
  // (on boards with PSRAM use ps_malloc instead)
  uint8_t* data = new (std::nothrow) uint8_t[len];
  if (data == nullptr) {
    Serial.print("no memory for ");
    Serial.print(len);
    Serial.println(" bytes");
    return false;
  }
  size_t n = 0;
  tinymobi::Error e = doc.readRecord(cover, data, len, n);
  tinymobi::ImageType type = tinymobi::imageType(data, n);
  const char* ext = extensionFor(type);
  bool ok = false;
  if (e == tinymobi::Error::None && ext != nullptr) {
    String path = String(kCoverPath) + ext;
    File out = SD.open(path.c_str(), FILE_WRITE);
    if (out) {
      out.write(data, n);
      out.close();
      Serial.print("cover saved to ");
      Serial.print(path);
      Serial.print(" (");
      Serial.print(n);
      Serial.println(" bytes)");
      ok = true;
    } else {
      Serial.println("cannot create the cover file");
    }
  } else {
    Serial.print("cover is not a known image: ");
    Serial.println(tinymobi::errorName(e));
  }
  delete[] data;
  return ok;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  if (!SD.begin(kSdCs)) {
    Serial.println("SD init failed");
    return;
  }
  File file = SD.open(kPath);
  if (!file) {
    Serial.println("file not found");
    return;
  }

  // the file stays open while the document is used
  tinymobi::FileMobi<File> doc(tinymobi::StreamSource<File>(file), tinymobi::TextMode::Plain);
  tinymobi::Error e = doc.open();
  if (e != tinymobi::Error::None) {
    Serial.print("open failed: ");
    Serial.println(tinymobi::errorName(e));
    return;
  }

  const tinymobi::Metadata& m = doc.metadata();
  Serial.print("Title : ");
  Serial.println(m.title);
  Serial.print("Author: ");
  Serial.println(m.author);

  saveCover(doc);

  // read the first 512 bytes of text
  uint8_t buf[512];
  size_t n = doc.read(buf, sizeof(buf));
  Serial.write(buf, n);
  Serial.println();

  // remember where we are, read on, then come back (bookmark / page turn)
  tinymobi::Position bookmark = doc.position();
  doc.read(buf, sizeof(buf));
  doc.seek(bookmark);
  Serial.print("bookmark at section ");
  Serial.print(bookmark.section);
  Serial.print(" offset ");
  Serial.println(bookmark.offset);
}

void loop() {}
