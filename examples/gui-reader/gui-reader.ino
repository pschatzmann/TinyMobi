/**
 * @file gui-reader.ino
 * @brief The book reader GUI (TinyMobi/MobiReaderApp.h) in a desktop SDL2
 * window. Mouse = touch. Without a book, the open dialog is shown; a book can
 * also be opened later from the menu.
 */
#include <SD.h>
#include <TinyMobi.h>
#include <TinyMobi/MobiReaderApp.h>

// a book that is opened at startup (optional) and the folder of the open dialog
#ifndef TINYMOBI_BOOK
#define TINYMOBI_BOOK ""
#endif
#ifndef TINYMOBI_LIBRARY
#define TINYMOBI_LIBRARY "/"
#endif

File file;
FileMobi<File> doc{StreamSource<File>(file)};
MobiReaderApp<RGB565, FileMobi<File>> gui(DefaultBoard, doc);

// the entries of a folder (the emulator's SD card uses host paths)
std::vector<tinymd::FileEntry> listDirectory(const std::string& dir) {
  std::vector<tinymd::FileEntry> entries;
  File folder = SD.open(dir.c_str());
  if (!folder || !folder.isDirectory()) return entries;
  for (File entry = folder.openNextFile(); entry; entry = folder.openNextFile()) {
    std::string name = entry.name();
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    entries.push_back(tinymd::FileEntry{name, entry.isDirectory()});
    entry.close();
  }
  folder.close();
  return entries;
}

std::string bookPath;  // the book that is open now

// Opens a book into the document. The book that is open stays open unless the
// new one is valid; if the new one still fails, the previous book is reopened.
bool openBook(const std::string& path) {
  {
    // check the candidate with a temporary document, so the open book is not touched yet
    File candidate = SD.open(path.c_str());
    FileMobi<File> check{StreamSource<File>(candidate)};
    if (!candidate || check.open() != Error::None) return false;
  }
  const std::string previous = bookPath;
  file.close();
  file = SD.open(path.c_str());
  if (file && doc.open() == Error::None) {
    bookPath = path;
    return true;
  }
  if (!previous.empty()) {
    file.close();
    file = SD.open(previous.c_str());
    doc.open();
    bookPath = previous;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  SD.begin(SS);
  gui.listFiles = listDirectory;
  gui.openBook = openBook;
  gui.libraryDir = TINYMOBI_LIBRARY;
  if (TINYMOBI_BOOK[0] != 0) openBook(TINYMOBI_BOOK);
  gui.begin();
}

void loop() {
  gui.update();
}
