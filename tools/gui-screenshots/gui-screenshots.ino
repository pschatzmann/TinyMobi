/**
 * @file gui-screenshots.ino
 * @brief Saves screenshots of the reader GUI (TinyMobi/MobiReaderApp.h) as BMP
 * images, for the documentation and for checking the rendering. No window is
 * shown: the cover, the next text pages and the settings screen are written
 * into the directory named by the environment variable TINYMOBI_SCREENSHOTS.
 *
 *   TINYMOBI_SCREENSHOTS=/path/to/dir ./gui-screenshots
 */
#include <functional>
#include <stdio.h>
#include <stdlib.h>

#include <SD.h>
#include <TinyMobi.h>
#include <TinyMobi/MobiReaderApp.h>

// the book on the host (the emulator's SD card uses host paths)
#ifndef TINYMOBI_BOOK
#define TINYMOBI_BOOK "/book.mobi"
#endif

File file;
tinymobi::FileMobi<File> doc{tinymobi::StreamSource<File>(file)};
tinymobi::MobiReaderApp<RGB565, tinymobi::FileMobi<File>> gui(DefaultBoard, doc);

// writes one screenshot as a BMP file into the directory dir
bool storeScreenshot(const char* dir, const char* name, const std::function<bool(Print&)>& write) {
  char path[256];
  snprintf(path, sizeof(path), "%s/%s.bmp", dir, name);
  File out = SD.open(path, FILE_WRITE);
  const bool ok = out && write(out);
  out.close();
  if (ok) printf("saved %s\n", path);
  return ok;
}

void setup() {
  Serial.begin(115200);
  SD.begin(SS);
  file = SD.open(TINYMOBI_BOOK);
  if (!file || doc.open() != tinymobi::Error::None) {
    Serial.println("cannot open the book");
    exit(1);
  }
  gui.begin();

  const char* dir = getenv("TINYMOBI_SCREENSHOTS");
  if (dir == nullptr) {
    Serial.println("set TINYMOBI_SCREENSHOTS to the directory for the images");
    exit(1);
  }
  gui.saveScreenshots([dir](const char* name, const std::function<bool(Print&)>& write) {
    return storeScreenshot(dir, name, write);
  });
  exit(0);
}

void loop() {}
