/*
 * TinyMobi - header-only MOBI / PalmDOC parser for Arduino and host C++.
 *
 * Designed for document readers on memory constrained MCUs:
 *  - The file is never loaded completely. The PalmDB record table, the
 *    headers and the metadata are read on demand through a small source
 *    adapter (memory buffer or any seekable file, e.g. SD / LittleFS / FFat).
 *  - Text is decompressed one record ("chunk") at a time into a single
 *    fixed size buffer (RecordBufferSize, default 4 KB). No heap is used.
 *  - Text can be streamed sequentially through read(), or random accessed
 *    per section through seek(), which makes page caching and
 *    bookmarks cheap.
 *  - Output is always UTF-8. CP1252 books are converted on the fly.
 *  - Optional plain text mode strips markup and decodes HTML entities.
 *
 * Supported: PalmDOC (LZ77), HUFF/CDIC and uncompressed text, trailing data
 * entries, EXTH metadata (title, author, publisher, language, date, cover),
 * the table of contents (NCX index), raw access to image records with image
 * type detection, UTF-8 and CP1252 text encodings.
 * KF8 (AZW3 and hybrid files): the parts are assembled from the skeleton and
 * fragment tables and read like any other text. Image references (recindex and
 * kindle:embed) are found with forEachImageRef().
 * Not supported: DRM encryption (reported as Error::Encrypted). The FDST
 * flow table is not parsed; the text is assumed to start at offset 0.
 *
 * Usage (memory buffer):
 *   MemoryMobi doc(MemorySource(data, len));
 *   if (doc.open() == Error::None) { ... doc.read(buf, n) ... }
 *
 * Usage (file):
 *   File f = SD.open("/book.mobi");
 *   FileMobi<File> doc(StreamSource<File>(f));
 *
 * License: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace tinymobi {

/// \brief Value used for "no such record / image".
constexpr uint32_t kNoRecord = 0xFFFFFFFFu;

/// \brief Error codes returned by the parser.
enum class Error : uint8_t {
  None = 0,
  Io,                    ///< the source could not deliver the requested bytes
  NotPalmDb,             ///< file is too small to be a PalmDB
  NotMobi,               ///< no PalmDOC/MOBI header found
  Encrypted,             ///< DRM protected book
  UnsupportedCompression,///< e.g. HUFF/CDIC (17480)
  CorruptData,           ///< inconsistent offsets or compressed stream
  BufferTooSmall,        ///< caller buffer or record buffer is too small
  OutOfRange,            ///< chunk / record index out of range
  NotOpen,               ///< open() was not called successfully
  Disabled,              ///< feature compiled out (WithKf8 = false)
};

/// \brief Returns a human readable name for an error (handy for Serial logs).
inline const char* errorName(Error e) {
  switch (e) {
    case Error::None: return "none";
    case Error::Io: return "io";
    case Error::NotPalmDb: return "not a PalmDB";
    case Error::NotMobi: return "not a MOBI file";
    case Error::Encrypted: return "encrypted (DRM)";
    case Error::UnsupportedCompression: return "unsupported compression";
    case Error::CorruptData: return "corrupt data";
    case Error::BufferTooSmall: return "buffer too small";
    case Error::OutOfRange: return "out of range";
    case Error::NotOpen: return "not open";
    case Error::Disabled: return "disabled in this configuration";
  }
  return "unknown";
}

/// \brief Output mode of the text stream.
enum class TextMode : uint8_t {
  Markup = 0, ///< raw book markup (HTML-like tags kept), UTF-8 encoded
  Plain = 1,  ///< tags removed, block tags become '\n', entities decoded
  /// Plain text where each link is marked: "\x01" + target + "\x02" + link text + "\x03".
  /// The target is the value that resolveLink() accepts. Links to other places
  /// (mailto:, http:) are not marked. Headings (h1-h6) are wrapped in
  /// "\x04" ... "\x05" on their own lines.
  PlainLinks = 2,
};

/// \brief Markers used by TextMode::PlainLinks.
enum : uint8_t {
  kLinkBegin = 0x01, kLinkTarget = 0x02, kLinkEnd = 0x03, kHeadBegin = 0x04, kHeadEnd = 0x05,
  kImageBegin = 0x06, kImageEnd = 0x07
};

/// \brief Position in the book text: a section (text record or KF8 part) and an offset in it.
class Position {
public:
  /// \brief Creates a position; both values default to the start of the book.
  Position(uint32_t s = 0, uint32_t o = 0) : section(s), offset(o) {}
  /// \brief True if both chunk and offset are equal.
  bool operator==(const Position& other) const {
    return section == other.section && offset == other.offset;
  }
  /// \brief True if chunk or offset differ.
  bool operator!=(const Position& other) const { return !(*this == other); }

  uint32_t section;  ///< section index (text record, or KF8 part)
  uint32_t offset;   ///< byte offset inside the section
};

/// \brief Technical information about the book.
struct Info {
  uint16_t compression = 0;          ///< 1 = none, 2 = PalmDOC
  bool utf8 = false;                 ///< text encoding is UTF-8 (else CP1252)
  bool isKf8 = false;                ///< MOBI version >= 8 (KF8 / AZW3)
  uint32_t mobiVersion = 0;          ///< MOBI header version (6 for MOBI, 8 for KF8)
  uint32_t textLength = 0;           ///< uncompressed size of all text (bytes)
  uint32_t textChunks = 0;           ///< number of text records
  uint32_t recordCount = 0;          ///< all PalmDB records
  uint16_t recordSize = 0;           ///< nominal uncompressed chunk size
  uint16_t extraFlags = 0;           ///< trailing entry flags
  uint32_t firstImageRecord = kNoRecord;  ///< PalmDB record of the first image
  uint32_t coverRecord = kNoRecord;  ///< absolute record index of the cover
  /// \brief Hybrid KF8 files: record index where the KF8 part starts (EXTH 121).
  /// Text chunks from there on belong to the KF8 part.
  uint32_t kf8Boundary = kNoRecord;
};

/// \brief Image formats that can appear as image records.
enum class ImageType : uint8_t { Unknown = 0, Jpeg, Png, Gif, Bmp };

/// \brief Detects the image format from the first bytes of a record.
inline ImageType imageType(const uint8_t* data, size_t len) {
  if (data == nullptr || len < 2) return ImageType::Unknown;
  if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return ImageType::Jpeg;
  if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) return ImageType::Png;
  if (len >= 6 && memcmp(data, "GIF87a", 6) == 0) return ImageType::Gif;
  if (len >= 6 && memcmp(data, "GIF89a", 6) == 0) return ImageType::Gif;
  if (data[0] == 'B' && data[1] == 'M') return ImageType::Bmp;
  return ImageType::Unknown;
}

/// \brief Text view: where a set of text records lives and how it is compressed.
struct TextView {
  uint32_t firstRec = 1;      ///< record index of the first text record
  uint32_t chunks = 0;        ///< number of text records
  uint32_t recordSize = 0;    ///< nominal uncompressed size of a record
  uint32_t textLength = 0;    ///< total uncompressed text length in bytes
  uint16_t compression = 0;   ///< 1 = none, 2 = PalmDOC, 17480 = HUFF/CDIC
  uint16_t extraFlags = 0;    ///< trailing entry flags of the records
};

/// \brief An image reference found in the text (recindex or kindle:embed).
struct ImageRef {
  Position position;    ///< where the tag starts (the '<')
  uint32_t record = kNoRecord;  ///< PalmDB record of the image, kNoRecord if invalid
};

/// \brief One entry of the table of contents (NCX index).
struct TocEntry {
  uint32_t index = 0;      ///< position of the entry in the table of contents
  Position start;          ///< where the section starts (section kNoRecord if unknown)
  uint32_t length = 0;     ///< length of the section in bytes
  uint8_t level = 0;       ///< nesting level, 0 = top level
  int32_t parent = -1;     ///< index of the parent entry or -1
  int32_t firstChild = -1; ///< index of the first child entry or -1
  int32_t lastChild = -1;  ///< index of the last child entry or -1
  char title[96] = {0};    ///< UTF-8 title
};

/// \brief Book metadata. Strings are UTF-8 and truncated to the buffer size.
struct Metadata {
  char title[128];      ///< book title (UTF-8)
  char author[96];      ///< author(s) (UTF-8)
  char publisher[64];   ///< publisher (UTF-8)
  char language[16];    ///< language code, e.g. "en"
  char published[16];   ///< publication date as stored in the book
};

namespace detail {

inline uint16_t be16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// \brief CP1252 0x80..0x9F mapping (0xA0..0xFF map 1:1 to Unicode).
inline uint32_t cp1252ToUnicode(uint8_t b) {
  static const uint16_t kHigh[32] = {
      0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
      0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
  if (b >= 0x80 && b < 0xA0) return kHigh[b - 0x80];
  return b;
}

inline size_t utf8Len(uint32_t cp) {
  return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

inline size_t utf8Encode(uint32_t cp, uint8_t* o) {
  if (cp < 0x80) {
    o[0] = static_cast<uint8_t>(cp);
    return 1;
  }
  if (cp < 0x800) {
    o[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
    o[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    o[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
    o[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    o[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    return 3;
  }
  o[0] = static_cast<uint8_t>(0xF0 | (cp >> 18));
  o[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
  o[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
  o[3] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  return 4;
}

/// \brief Removes an incomplete UTF-8 sequence at the end of s[0..o).
inline size_t trimIncompleteUtf8(const char* s, size_t o) {
  size_t i = o;
  for (int back = 0; i > 0 && back < 4; ++back) {
    --i;
    uint8_t c = static_cast<uint8_t>(s[i]);
    if ((c & 0xC0) != 0x80) {  // lead byte or ASCII
      size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      return (i + need > o) ? i : o;
    }
  }
  return o;
}

/// \brief A named HTML entity (e.g. "amp") and its Unicode code point.
struct NamedEntity {
  const char* name;  ///< entity name without '&' and ';'
  uint16_t cp;       ///< Unicode code point
};

inline bool lookupEntity(const uint8_t* s, size_t n, uint32_t& cp) {
  static const NamedEntity kEntities[] = {
      {"amp", '&'},        {"lt", '<'},          {"gt", '>'},
      {"quot", '"'},       {"apos", '\''},       {"nbsp", 0xA0},
      {"copy", 0xA9},      {"reg", 0xAE},        {"deg", 0xB0},
      {"middot", 0xB7},    {"laquo", 0xAB},      {"raquo", 0xBB},
      {"times", 0xD7},     {"szlig", 0xDF},      {"aacute", 0xE1},
      {"agrave", 0xE0},    {"auml", 0xE4},       {"eacute", 0xE9},
      {"egrave", 0xE8},    {"ouml", 0xF6},       {"uuml", 0xFC},
      {"ndash", 0x2013},   {"mdash", 0x2014},    {"lsquo", 0x2018},
      {"rsquo", 0x2019},   {"ldquo", 0x201C},    {"rdquo", 0x201D},
      {"bull", 0x2022},    {"hellip", 0x2026},   {"euro", 0x20AC},
  };
  if (n == 0) return false;
  if (s[0] == '#') {  // numeric reference: &#NNN; or &#xHH;
    size_t i = 1;
    uint32_t base = 10;
    if (n > 1 && (s[1] == 'x' || s[1] == 'X')) {
      base = 16;
      i = 2;
    }
    if (i >= n) return false;
    uint32_t v = 0;
    for (; i < n; ++i) {
      uint8_t c = s[i];
      uint32_t d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else return false;
      v = v * base + d;
      if (v > 0x10FFFF) return false;
    }
    if (v == 0) return false;
    cp = v;
    return true;
  }
  for (const NamedEntity& e : kEntities) {
    size_t len = strlen(e.name);
    if (len == n && memcmp(e.name, s, n) == 0) {
      cp = e.cp;
      return true;
    }
  }
  return false;
}

inline char lowerAscii(uint8_t c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
}

inline bool isAsciiAlpha(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Source adapters. A source provides:
//   uint32_t size();
//   size_t   read(uint32_t pos, void* buf, size_t n);  // returns bytes read
// ---------------------------------------------------------------------------

/// \brief Source backed by a memory buffer that stays owned by the caller.
class MemorySource {
public:
  /// \brief Wraps a memory buffer. The buffer must stay valid while the document is used.
  MemorySource(const uint8_t* data, size_t length) : _data(data), _length(length) {}

  /// \brief Size of the buffer in bytes.
  uint32_t size() const { return static_cast<uint32_t>(_length); }

  /// \brief Copies up to n bytes starting at pos. Returns the number of bytes copied.
  size_t read(uint32_t pos, void* buf, size_t n) {
    if (pos >= _length) return 0;
    size_t avail = _length - pos;
    if (n > avail) n = avail;
    memcpy(buf, _data + pos, n);
    return n;
  }

private:
  const uint8_t* _data;
  size_t _length;
};

/// \brief Source backed by any seekable file with size(), seek(pos) and read(buf, n),
/// e.g. Arduino File (SD, LittleFS, FFat, SPIFFS), SdFat FsFile, ...
/// The file must stay open while the document is used.
template <typename File>
class StreamSource {
public:
  /// \brief Wraps an open file. The file must stay open while the document is used.
  explicit StreamSource(File& file) : _file(file) {}

  /// \brief Size of the file in bytes.
  uint32_t size() { return static_cast<uint32_t>(_file.size()); }

  /// \brief Seeks to pos and reads up to n bytes. Returns the number of bytes read.
  size_t read(uint32_t pos, void* buf, size_t n) {
    if (!_file.seek(pos)) return 0;
    auto r = _file.read(static_cast<uint8_t*>(buf), n);
    return r > 0 ? static_cast<size_t>(r) : 0;
  }

private:
  File& _file;
};

// ---------------------------------------------------------------------------
// The document parser
// ---------------------------------------------------------------------------

/// \brief MOBI document parser and text stream.
///
/// @tparam Source            one of MemorySource, StreamSource<File> or a
///                           custom type with the interface described above
/// @tparam RecordBufferSize  size of the decompression buffer in bytes. It
///                           must be >= the largest uncompressed text record
///                           (4096 for virtually all books).
template <typename Source, size_t RecordBufferSize = 4096, bool WithKf8 = true>
class MobiDocument {
  static_assert(RecordBufferSize >= 512, "RecordBufferSize too small");

public:
  /// \brief Creates a document on a source. Call open() before using it.
  explicit MobiDocument(Source source, TextMode mode = TextMode::Markup)
      : _src(source), _mode(mode) {}

  // -- lifecycle -----------------------------------------------------------

  /// \brief Parses the PalmDB, PalmDOC and MOBI headers and the EXTH metadata.
  /// Only a few hundred bytes are read; text is decoded lazily.
  Error open() {
    close();
    _total = _src.size();
    uint8_t head[78];
    if (_total < 78 || _src.read(0, head, sizeof(head)) != sizeof(head)) {
      return fail(Error::NotPalmDb);
    }
    if (memcmp(head + 64, "MOBI", 4) != 0) return fail(Error::NotMobi);
    _numRecords = detail::be16(head + 76);
    if (_numRecords == 0) return fail(Error::NotMobi);
    _info.recordCount = _numRecords;

    // record 0 holds the PalmDOC and MOBI headers
    _rec0 = recordOffset(0);
    uint32_t end0 = recordEnd(0);
    if (_rec0 == kNoRecord || end0 == kNoRecord || end0 > _total || end0 <= _rec0) {
      return fail(Error::CorruptData);
    }
    _rec0Len = end0 - _rec0;

    uint8_t magic[4];
    if (!readRel(16, magic, 4) || memcmp(magic, "MOBI", 4) != 0) {
      return fail(Error::NotMobi);
    }
    uint32_t headerLen = 0;
    if (!rd32(20, headerLen)) return fail(Error::CorruptData);
    _mobiHeaderLen = headerLen;

    uint32_t comp = 0, textLen = 0, recCount = 0, recSize = 0, enc = 0;
    if (!rd16(0, comp) || !rd32(4, textLen) || !rd16(8, recCount) ||
        !rd16(10, recSize) || !rd16(12, enc)) {
      return fail(Error::CorruptData);
    }
    if (enc != 0) return fail(Error::Encrypted);
    if (comp != 1 && comp != 2 && comp != kHuffCompression) {
      return fail(Error::UnsupportedCompression);
    }

    _info = Info();
    memset(&_meta, 0, sizeof(_meta));
    _info.recordCount = _numRecords;
    _info.compression = static_cast<uint16_t>(comp);
    _info.textLength = textLen;
    _info.recordSize = static_cast<uint16_t>(recSize);
    _info.textChunks = recCount < _numRecords - 1 ? recCount : _numRecords - 1;
    _info.firstImageRecord = kNoRecord;
    _info.coverRecord = kNoRecord;

    uint32_t encoding = 0;
    if (mobiField(28, encoding)) _utf8 = (encoding == 65001);
    _info.utf8 = _utf8;

    uint32_t version = 0;
    if (mobiField(0x68, version)) {
      _info.mobiVersion = version;
      _info.isKf8 = version >= 8;
    }

    uint32_t firstImage = 0;
    if (mobiField(0x6C, firstImage) && firstImage != 0 && firstImage < _numRecords) {
      _info.firstImageRecord = firstImage;
    }

    uint32_t extra = 0;
    if (headerLen + 16 >= 0xF4 && rd16(0xF2, extra)) {
      _info.extraFlags = static_cast<uint16_t>(extra);
    }

    if (comp == kHuffCompression) {
      uint32_t huffRec = 0, huffCount = 0;
      if (!mobiField(0x70, huffRec) || !mobiField(0x74, huffCount)) {
        return fail(Error::CorruptData);
      }
      Error he = loadHuffTables(huffRec, huffCount);
      if (he != Error::None) return fail(he);
    }

    // full title, located in record 0
    uint32_t nameOff = 0, nameLen = 0;
    if (mobiField(0x54, nameOff) && mobiField(0x58, nameLen) && nameLen > 0 &&
        nameOff < _rec0Len && nameLen <= _rec0Len - nameOff) {
      copyText(_rec0 + nameOff, nameLen, _meta.title, sizeof(_meta.title));
    }

    uint32_t exthFlags = 0;
    if (mobiField(0x80, exthFlags) && (exthFlags & 0x40) != 0) {
      parseExth();
    }

    _mobi6View.firstRec = 1;
    _mobi6View.chunks = _info.textChunks;
    _mobi6View.recordSize = _info.recordSize;
    _mobi6View.textLength = _info.textLength;
    _mobi6View.compression = _info.compression;
    _mobi6View.extraFlags = _info.extraFlags;
    _kf8Loaded = false;
    _partOpen = false;
    _walkSection = 0;
    _walkStart = 0;
    _rawChunk = kNoRecord;

    _open = true;
    // KF8 text is used when the book has the KF8 parts (AZW3 and hybrid books)
    _useKf8 = ensureKf8() == Error::None && _kf8Skel != kNoRecord && _kf8Frag != kNoRecord;
    _error = Error::None;
    rewind();
    return Error::None;
  }

  /// \brief Releases the parsed state. The source is not closed.
  void close() {
    _open = false;
    _partOpen = false;
    _skipping = false;
    _recLen = _recPos = 0;
    _pendLen = _pendOff = 0;
    _inTag = false;
    _tagNameLen = 0;
    _tagNameDone = false;
    _lastOut = '\n';
    _error = Error::None;
  }

  /// \brief True after a successful open().
  bool isOpen() const { return _open; }
  /// \brief The error of the last failed call.
  Error lastError() const { return _error; }

  // -- information -----------------------------------------------------------

  /// \brief Technical information (valid after open()).
  const Info& info() const { return _info; }
  /// \brief Title, author and other metadata (valid after open()).
  const Metadata& metadata() const { return _meta; }

  /// \brief Total number of PalmDB records (text, images, ...).
  uint32_t recordCount() const { return _info.recordCount; }

  // -- random access ---------------------------------------------------------

  /// \brief Reads a raw PalmDB record, e.g. an image (JPEG/PNG/GIF) or the cover.
  Error readRecord(uint32_t index, uint8_t* dst, size_t cap, size_t& outLen) {
    outLen = 0;
    if (!_open) return fail(Error::NotOpen);
    if (index >= _numRecords) return fail(Error::OutOfRange);
    uint32_t start = recordOffset(index);
    uint32_t end = recordEnd(index);
    if (start == kNoRecord || end == kNoRecord || end > _total || end < start) {
      return fail(Error::CorruptData);
    }
    uint32_t len = end - start;
    if (len > cap) return fail(Error::BufferTooSmall);
    if (len > 0 && _src.read(start, dst, len) != len) return fail(Error::Io);
    outLen = len;
    return Error::None;
  }

  // -- table of contents ----------------------------------------------------

  // -- image references ------------------------------------------------------

  /// \brief Calls fn(const ImageRef&) for every image tag of the book, in reading
  /// order. fn returns false to stop early. The reading position is kept.
  template <typename Fn>
  Error forEachImageRef(Fn fn) {
    if (!_open) return fail(Error::NotOpen);
    Position saved = position();
    Error e = rewind();
    if (e != Error::None) return e;
    uint8_t tag[256];
    size_t tagLen = 0;
    bool inTag = false;
    bool stop = false;
    Position tagStart;
    // the buffer loaded by rewind() is scanned too, then each following one
    for (bool more = true; more && !stop; more = loadNext()) {
      for (uint32_t k = 0; k < _recLen && !stop; ++k) {
        uint8_t b = _rec[k];
        if (!inTag) {
          if (b == '<') {
            inTag = true;
            tagLen = 0;
            tagStart = Position(_curChunk, _secBase + k);
          }
          continue;
        }
        if (b == '>') {
          inTag = false;
          uint32_t number = 0;
          if (parseImageTag(tag, tagLen, number)) {
            ImageRef ref;
            ref.position = tagStart;
            ref.record = imageRecordOf(number);
            if (!fn(ref)) stop = true;
          }
          continue;
        }
        if (tagLen < sizeof(tag)) {
          tag[tagLen++] = b;
        } else {
          inTag = false;  // too long to be an image tag
        }
      }
    }
    seek(saved);  // the reading position is kept
    return Error::None;
  }

  /// \brief Number of table of contents entries (0 if the book has none).
  Error tocCount(uint32_t& count) {
    count = 0;
    if (!_open) return fail(Error::NotOpen);
    uint32_t ncx = kNoRecord;
    IndxHeader mh;
    Error e = openNcx(ncx, mh);
    if (e != Error::None || ncx == kNoRecord) return e == Error::None ? e : fail(e);
    for (uint32_t i = 1; i <= mh.count; ++i) {
      IndxHeader ih;
      e = readIndxHeader(ncx + i, ih);
      if (e != Error::None) return fail(e);
      count += ih.count;
    }
    return Error::None;
  }

  /// \brief Calls fn(const TocEntry&) for each table of contents entry in order.
  /// fn returns false to stop early.
  template <typename Fn>
  Error forEachTocEntry(Fn fn) {
    if (!_open) return fail(Error::NotOpen);
    uint32_t ncx = kNoRecord;
    IndxHeader mh;
    Error e = openNcx(ncx, mh);
    if (e != Error::None) return fail(e);
    if (ncx == kNoRecord) return Error::None;
    TagTable tt;
    e = readTagTable(ncx, mh, tt);
    if (e != Error::None) return e;
    uint32_t index = 0;
    for (uint32_t i = 1; i <= mh.count; ++i) {
      IndxHeader ih;
      e = readIndxHeader(ncx + i, ih);
      if (e != Error::None) return fail(e);
      for (uint32_t j = 0; j < ih.count; ++j) {
        TocEntry te;
        e = readTocEntry(ncx + i, ih, j, tt, ncx, mh, index, te);
        if (e != Error::None) return fail(e);
        ++index;
        if (!fn(te)) return Error::None;
      }
    }
    return Error::None;
  }

  /// \brief Returns the entry with the given index.
  Error tocEntry(uint32_t index, TocEntry& out) {
    bool found = false;
    Error e = forEachTocEntry([&](const TocEntry& te) -> bool {
      if (te.index == index) {
        out = te;
        found = true;
        return false;
      }
      return true;
    });
    if (e != Error::None) return e;
    return found ? Error::None : fail(Error::OutOfRange);
  }

  /// \brief Size in bytes of a raw PalmDB record (e.g. the cover, see readRecord()).
  Error recordLength(uint32_t index, uint32_t& len) {
    len = 0;
    if (!_open) return fail(Error::NotOpen);
    if (index >= _numRecords) return fail(Error::OutOfRange);
    uint32_t start = recordOffset(index), end = recordEnd(index);
    if (start == kNoRecord || end == kNoRecord || end > _total || end < start) {
      return fail(Error::CorruptData);
    }
    len = end - start;
    return Error::None;
  }

  /// \brief Moves the text stream to the start of the book.
  Error rewind() { return seek(Position(0, 0)); }

  /// \brief Moves the text stream to a position previously returned by position().
  /// Plain text state (inside a tag) is reset, so seeking in the middle of a tag
  /// may show a few characters of markup. KF8 parts are rebuilt up to the offset.
  Error seek(Position p) {
    if (!_open) return fail(Error::NotOpen);
    if (p.section >= sectionCount()) return fail(Error::OutOfRange);
    _pendLen = _pendOff = 0;
    _inTag = false;
    _tagNameDone = false;
    _tagNameLen = 0;
    _lastOut = '\n';
    _partOpen = false;
    _skipping = false;
    _tagClose = false;
    _attrLen = 0;
    _linkOpen = false;
    _nextChunk = p.section;
    if (!loadNext()) return _error;
    while (_useKf8 && _secBase + _recLen < p.offset) {
      if (!kfFill()) break;
    }
    _recPos = p.offset > _secBase ? p.offset - _secBase : 0;
    if (_recPos > _recLen) _recPos = _recLen;
    return Error::None;
  }

  /// \brief Current position of the text stream (valid for seek()).
  Position position() const { return Position(_curChunk, _secBase + _recPos); }

  /// \brief Selects markup or plain text output. Takes effect for the next byte.
  void setTextMode(TextMode mode) {
    _mode = mode;
    _inTag = false;
    _linkOpen = false;
  }

  /// \brief The current text output mode.
  TextMode textMode() const { return _mode; }

  /// \brief Resolves a link target from TextMode::PlainLinks (the text between
  /// kLinkBegin and kLinkTarget) to a position in the text. Returns false if the
  /// target does not exist or cannot be resolved. MOBI6 targets are "filepos=N";
  /// KF8 targets are "kindle:pos:fid:XXXX:off:YYYYYYYYYY" (the fragment table maps
  /// them to the part and offset).
  bool resolveLink(const char* target, Position& out) {
    if (!_open || target == nullptr) return false;
    static const char kFilepos[] = "filepos=";
    const size_t fl = sizeof(kFilepos) - 1;
    if (strncmp(target, kFilepos, fl) == 0) {
      uint32_t v = 0;
      for (const char* c = target + fl; *c != 0; ++c) {
        if (*c < '0' || *c > '9') return false;
        v = v * 10 + static_cast<uint32_t>(*c - '0');
      }
      Position p = textPosition(v);
      if (p.section == kNoRecord) return false;
      out = p;
      return true;
    }
    return resolveKf8Link(target, out);
  }

  // -- sequential text stream ------------------------------------------------

  /// \brief Reads up to n bytes of UTF-8 text. Returns the number of bytes copied;
  /// 0 means the end of the book (or an error, see lastError()).
  size_t read(uint8_t* dst, size_t n) {
    if (!_open || dst == nullptr) return 0;
    size_t out = 0;
    while (out < n) {
      if (_pendOff < _pendLen) {
        dst[out++] = _pend[_pendOff++];
        continue;
      }
      // fast path: markup and UTF-8 book -> plain memcpy from the chunk
      if (_mode == TextMode::Markup && _utf8) {
        while (_recPos >= _recLen) {
          if (!loadNext()) return out;
        }
        size_t avail = _recLen - _recPos;
        size_t count = avail < n - out ? avail : n - out;
        memcpy(dst + out, _rec + _recPos, count);
        _recPos += static_cast<uint32_t>(count);
        out += count;
        _lastOut = dst[out - 1];
        continue;
      }
      _pendOff = _pendLen = 0;
      if (!produce()) break;
    }
    return out;
  }

private:
  // -- helpers: reads relative to record 0 -------------------------------

  // -- HUFF/CDIC (compression 17480) ----------------------------------------

  enum : uint32_t { kHuffCompression = 17480, kMaxPhraseDepth = 16 };

  /// \brief Cache of source bytes for the HUFF bit reader.
  struct BitWindow {
    uint32_t base;   ///< source offset of buf[0]
    uint32_t len;    ///< valid bytes in buf
    uint8_t buf[16];
  };

  bool readAbs(uint32_t pos, void* dst, size_t n) { return _src.read(pos, dst, n) == n; }

  static uint32_t countBits(uint8_t v) {
    uint32_t n = 0;
    for (; v != 0; v = static_cast<uint8_t>(v >> 1)) n += v & 1;
    return n;
  }

  /// \brief Reads the byte at absolute offset i of the slice, using a small window.
  bool windowByte(BitWindow& w, uint32_t srcStart, uint32_t srcLen, uint32_t i, uint8_t& b) {
    if (i < w.base || i >= w.base + w.len) {
      uint32_t want = srcLen - i < sizeof(w.buf) ? srcLen - i : static_cast<uint32_t>(sizeof(w.buf));
      if (!readAbs(srcStart + i, w.buf, want)) return false;
      w.base = i;
      w.len = want;
    }
    b = w.buf[i - w.base];
    return true;
  }

  /// \brief Returns the 32 bits starting at bitPos (MSB first), zero padded at the end.
  bool peekBits32(BitWindow& w, uint32_t srcStart, uint32_t srcLen, uint64_t bitPos,
                  uint32_t& code) {
    uint32_t byteIdx = static_cast<uint32_t>(bitPos >> 3);
    uint32_t shift = static_cast<uint32_t>(bitPos & 7);
    uint64_t v = 0;
    for (uint32_t k = 0; k < 5; ++k) {
      uint8_t b = 0;
      uint32_t i = byteIdx + k;
      if (i < srcLen && !windowByte(w, srcStart, srcLen, i, b)) return false;
      v = (v << 8) | b;
    }
    code = static_cast<uint32_t>((v >> (8 - shift)) & 0xFFFFFFFFu);
    return true;
  }

  uint64_t huffMinCode(uint32_t len) const {
    return static_cast<uint64_t>(_dict2[2 * (len - 1)]) << (32 - len);
  }

  uint64_t huffMaxCode(uint32_t len) const {
    return ((static_cast<uint64_t>(_dict2[2 * (len - 1) + 1]) + 1) << (32 - len)) - 1;
  }

  /// \brief Reads count big endian 32 bit values at base + relOff, in small blocks.
  bool loadBE32Table(uint32_t base, uint32_t relOff, uint32_t recLen, uint32_t* dst,
                     uint32_t count) {
    if (relOff > recLen || count * 4 > recLen - relOff) return false;
    uint8_t t[64];
    for (uint32_t done = 0; done < count;) {
      uint32_t n = count - done < 16 ? count - done : 16;
      if (!readAbs(base + relOff + 4 * done, t, 4 * n)) return false;
      for (uint32_t k = 0; k < n; ++k) dst[done + k] = detail::be32(t + 4 * k);
      done += n;
    }
    return true;
  }

  Error loadHuffTables(uint32_t huffRec, uint32_t huffCount) {
    if (huffRec == 0 || huffCount < 2 || huffRec >= _numRecords ||
        huffCount > _numRecords - huffRec) {
      return fail(Error::CorruptData);
    }
    uint32_t start = recordOffset(huffRec), end = recordEnd(huffRec);
    if (start == kNoRecord || end == kNoRecord || end > _total || end < start ||
        end - start < 24) {
      return fail(Error::CorruptData);
    }
    uint8_t h[24];
    if (!readAbs(start, h, sizeof(h))) return fail(Error::Io);
    if (memcmp(h, "HUFF\0\0\0\x18", 8) != 0) return fail(Error::CorruptData);
    uint32_t off1 = detail::be32(h + 8), off2 = detail::be32(h + 12);
    uint32_t recLen = end - start;
    if (!loadBE32Table(start, off1, recLen, _dict1, 256) ||
        !loadBE32Table(start, off2, recLen, _dict2, 64)) {
      return fail(Error::CorruptData);
    }

    // the CDIC records follow the HUFF record
    uint32_t cs = recordOffset(huffRec + 1), ce = recordEnd(huffRec + 1);
    uint8_t c[16];
    if (cs == kNoRecord || ce == kNoRecord || ce > _total || ce < cs || ce - cs < 16 ||
        !readAbs(cs, c, sizeof(c)) || memcmp(c, "CDIC\0\0\0\x10", 8) != 0) {
      return fail(Error::CorruptData);
    }
    _phrases = detail::be32(c + 8);
    _cdicBits = detail::be32(c + 12);
    if (_cdicBits == 0 || _cdicBits > 16) return fail(Error::CorruptData);
    _cdicFirst = huffRec + 1;
    _cdicCount = huffCount - 1;
    return Error::None;
  }

  /// \brief Decodes one dictionary phrase. Stored phrases are copied, compressed
  /// phrases are decoded recursively.
  Error huffPhrase(uint32_t r, uint8_t* out, size_t cap, size_t& o, uint32_t depth) {
    uint32_t cdic = r >> _cdicBits;
    uint32_t slot = r & ((1u << _cdicBits) - 1);
    if (cdic >= _cdicCount) return fail(Error::CorruptData);
    uint32_t rec = _cdicFirst + cdic;
    uint32_t start = recordOffset(rec), end = recordEnd(rec);
    if (start == kNoRecord || end == kNoRecord || end > _total || end < start) {
      return fail(Error::CorruptData);
    }
    uint32_t recLen = end - start;
    uint8_t b2[2];
    if (16 + 2 * slot + 2 > recLen || !readAbs(start + 16 + 2 * slot, b2, 2)) {
      return fail(Error::CorruptData);
    }
    uint32_t off = detail::be16(b2);
    if (16 + off + 2 > recLen || !readAbs(start + 16 + off, b2, 2)) {
      return fail(Error::CorruptData);
    }
    uint32_t blen = detail::be16(b2);
    uint32_t plen = blen & 0x7FFF;
    uint32_t dataRel = 18 + off;
    if (dataRel + plen > recLen) return fail(Error::CorruptData);
    if ((blen & 0x8000) != 0) {  // stored as plain text
      if (o + plen > cap) return fail(Error::BufferTooSmall);
      if (!readAbs(start + dataRel, out + o, plen)) return fail(Error::Io);
      o += plen;
      return Error::None;
    }
    return huffUnpack(start + dataRel, plen, out, cap, o, depth + 1);
  }

  /// \brief Huffman decoding of one slice (a record or a compressed phrase).
  /// Each code is an index into the dictionary.
  Error huffUnpack(uint32_t srcStart, uint32_t srcLen, uint8_t* out, size_t cap, size_t& o,
                   uint32_t depth) {
    if (depth > kMaxPhraseDepth) return fail(Error::CorruptData);
    BitWindow w;
    w.base = 0;
    w.len = 0;
    const uint64_t totalBits = static_cast<uint64_t>(srcLen) * 8;
    uint64_t bitPos = 0;
    while (bitPos < totalBits) {
      uint32_t code = 0;
      if (!peekBits32(w, srcStart, srcLen, bitPos, code)) return fail(Error::Io);
      uint32_t v = _dict1[code >> 24];
      uint32_t codelen = v & 0x1F;
      if (codelen == 0) return fail(Error::CorruptData);
      uint64_t maxcode = ((static_cast<uint64_t>(v >> 8) + 1) << (32 - codelen)) - 1;
      if ((v & 0x80) == 0) {  // not terminal in the first table: search the length
        while (codelen <= 32 && static_cast<uint64_t>(code) < huffMinCode(codelen)) ++codelen;
        if (codelen > 32) return fail(Error::CorruptData);
        maxcode = huffMaxCode(codelen);
      }
      if (bitPos + codelen > totalBits) break;  // padding at the end of the slice
      bitPos += codelen;
      if (maxcode < code) return fail(Error::CorruptData);
      uint64_t r = (maxcode - code) >> (32 - codelen);
      if (r >= _phrases) return fail(Error::CorruptData);
      Error e = huffPhrase(static_cast<uint32_t>(r), out, cap, o, depth);
      if (e != Error::None) return e;
    }
    return Error::None;
  }

  // -- NCX index (table of contents) -----------------------------------------

  enum : uint32_t { kMaxTags = 32, kMaxTocEntryBytes = 256 };

  /// \brief Header of an INDX record (NCX, skeleton, fragment and CTOC indexes).
  struct IndxHeader {
    uint32_t headerLen = 0;
    uint32_t count = 0;   // extra INDX records (main) or entries (IDXT records)
    uint32_t start = 0;   // IDXT offset (IDXT records)
    uint32_t nctoc = 0;   // CTOC records (main record)
  };

  /// \brief One entry of the TAGX table: which tag, how many values, and its bit mask.
  struct TagEntry {
    uint8_t tag, vpe, mask, endFlag;
  };

  /// \brief The TAGX table of an index record.
  struct TagTable {
    uint32_t controlBytes = 0;
    uint32_t count = 0;
    TagEntry e[kMaxTags];
  };

  /// \brief Values of the tags of one index entry (only tags below kMaxTags are kept).
  struct TagMap {
    bool has[kMaxTags];
    uint8_t n[kMaxTags];
    uint32_t v[kMaxTags][2];
  };

  Error readIndxHeader(uint32_t rec, IndxHeader& h) {
    uint32_t start = recordOffset(rec), end = recordEnd(rec);
    if (start == kNoRecord || end == kNoRecord || end > _total || end < start ||
        end - start < 56) {
      return fail(Error::CorruptData);
    }
    uint8_t b[56];
    if (!readAbs(start, b, sizeof(b))) return fail(Error::Io);
    if (memcmp(b, "INDX", 4) != 0) return fail(Error::CorruptData);
    h.headerLen = detail::be32(b + 4);
    h.start = detail::be32(b + 20);
    h.count = detail::be32(b + 24);
    h.nctoc = detail::be32(b + 52);
    return Error::None;
  }

  /// \brief Locates the NCX index record (MOBI header offset 0xF4), if the book has one.
  Error openNcx(uint32_t& rec, IndxHeader& mh) {
    rec = kNoRecord;
    uint32_t idx = 0;
    if (!mobiField(0xF4, idx) || idx == 0 || idx >= _numRecords) return Error::None;
    Error e = readIndxHeader(idx, mh);
    if (e != Error::None) return e;
    rec = idx;
    return Error::None;
  }

  /// \brief Reads the TAGX section that follows the header of the main index record.
  Error readTagTable(uint32_t rec, const IndxHeader& mh, TagTable& t) {
    t.controlBytes = 0;
    t.count = 0;
    uint32_t start = recordOffset(rec);
    uint8_t b[12];
    if (!readAbs(start + mh.headerLen, b, sizeof(b))) return fail(Error::Io);
    if (memcmp(b, "TAGX", 4) != 0) return Error::None;  // no tags
    uint32_t first = detail::be32(b + 4);
    t.controlBytes = detail::be32(b + 8);
    if (first < 12 || (first - 12) % 4 != 0 || (first - 12) / 4 > kMaxTags) {
      return fail(Error::CorruptData);
    }
    uint32_t n = (first - 12) / 4;
    uint8_t tb[4 * kMaxTags];
    if (!readAbs(start + mh.headerLen + 12, tb, 4 * n)) return fail(Error::Io);
    for (uint32_t k = 0; k < n; ++k) {
      t.e[k].tag = tb[4 * k];
      t.e[k].vpe = tb[4 * k + 1];
      t.e[k].mask = tb[4 * k + 2];
      t.e[k].endFlag = tb[4 * k + 3];
    }
    t.count = n;
    return Error::None;
  }

  /// \brief Reads a variable width integer (7 bits per byte, high bit ends it).
  static bool readVwi(const uint8_t* d, uint32_t len, uint32_t& pos, uint32_t& value) {
    value = 0;
    for (uint32_t k = 0; k < 5; ++k) {
      if (pos >= len) return false;
      uint8_t b = d[pos++];
      value = (value << 7) | (b & 0x7F);
      if ((b & 0x80) != 0) return true;
    }
    return false;
  }

  /// \brief Decodes the tag values of one index entry. Only the first two values of
  /// each tag below kMaxTags are kept.
  static bool parseTagMap(const uint8_t* d, uint32_t len, uint32_t startPos, const TagTable& tt,
                          TagMap& m) {
    memset(&m, 0, sizeof(m));
    struct Pending {
      uint8_t tag;
      uint8_t vpe;     // values per group
      int32_t count;   // number of value groups, or -1 if 'bytes' is used
      uint32_t bytes;  // size of the value data in bytes
    };
    Pending pend[kMaxTags];
    uint32_t np = 0;
    uint32_t cb = 0;
    uint32_t dataPos = startPos + tt.controlBytes;
    for (uint32_t k = 0; k < tt.count; ++k) {
      const TagEntry& te = tt.e[k];
      if (te.endFlag == 1) {
        ++cb;
        continue;
      }
      if (startPos + cb >= len) return false;
      uint8_t mask = te.mask;
      uint8_t value = d[startPos + cb] & mask;
      if (value == 0) continue;
      Pending& pe = pend[np++];
      pe.tag = te.tag;
      pe.vpe = te.vpe;
      pe.bytes = 0;
      if (value == mask) {
        if (countBits(mask) > 1) {
          uint32_t v = 0;
          if (!readVwi(d, len, dataPos, v)) return false;
          pe.count = -1;
          pe.bytes = v;
        } else {
          pe.count = 1;
        }
      } else {
        while ((mask & 1) == 0) {
          mask = static_cast<uint8_t>(mask >> 1);
          value = static_cast<uint8_t>(value >> 1);
        }
        pe.count = value;
      }
    }
    for (uint32_t p = 0; p < np; ++p) {
      const Pending& pe = pend[p];
      uint32_t idx = 0;
      auto take = [&](uint32_t val) {
        if (pe.tag < kMaxTags && idx < 2) m.v[pe.tag][idx] = val;
        ++idx;
      };
      if (pe.count >= 0) {
        for (int32_t c = 0; c < pe.count; ++c) {
          for (uint8_t j = 0; j < pe.vpe; ++j) {
            uint32_t v = 0;
            if (!readVwi(d, len, dataPos, v)) return false;
            take(v);
          }
        }
      } else {
        uint32_t consumed = 0;
        while (consumed < pe.bytes) {
          uint32_t before = dataPos;
          uint32_t v = 0;
          if (!readVwi(d, len, dataPos, v)) return false;
          consumed += dataPos - before;
          take(v);
        }
      }
      if (pe.tag < kMaxTags) {
        m.has[pe.tag] = true;
        m.n[pe.tag] = static_cast<uint8_t>(idx < 2 ? idx : 2);
      }
    }
    return true;
  }

  /// \brief Title of a TOC entry: the CTOC record encodes noffs as (record << 16 | offset).
  void ctocTitle(const IndxHeader& mh, uint32_t ncx, uint32_t noffs, char* dst, size_t cap) {
    dst[0] = 0;
    uint32_t j = noffs >> 16;
    if (j >= mh.nctoc) return;
    uint32_t rec = ncx + mh.count + 1 + j;
    uint32_t start = recordOffset(rec), end = recordEnd(rec);
    if (start == kNoRecord || end == kNoRecord || end > _total || end < start) return;
    uint32_t recLen = end - start;
    uint32_t off = noffs & 0xFFFF;
    if (off >= recLen) return;
    uint8_t v[5];
    uint32_t n = recLen - off < 5 ? recLen - off : 5;
    if (!readAbs(start + off, v, n)) return;
    uint32_t pos = 0, len = 0;
    if (!readVwi(v, n, pos, len)) return;
    if (pos + len > recLen - off) return;
    copyText(start + off + pos, len, dst, cap);
  }

  /// \brief Reads one entry of an index record: its label and its tag values.
  Error readIndexEntry(uint32_t rec, const IndxHeader& ih, uint32_t j, const TagTable& tt,
                       char* label, size_t labelCap, TagMap& m) {
    label[0] = 0;
    uint32_t start = recordOffset(rec), end = recordEnd(rec);
    if (start == kNoRecord || end == kNoRecord || end > _total || end < start) {
      return Error::CorruptData;
    }
    uint32_t recLen = end - start;
    uint8_t p[2];
    if (ih.start + 4 + 2 * j + 2 > recLen || !readAbs(start + ih.start + 4 + 2 * j, p, 2)) {
      return Error::CorruptData;
    }
    uint32_t startPos = detail::be16(p);
    uint32_t endPos = ih.start;
    if (j + 1 < ih.count) {
      if (ih.start + 4 + 2 * (j + 1) + 2 > recLen ||
          !readAbs(start + ih.start + 4 + 2 * (j + 1), p, 2)) {
        return Error::CorruptData;
      }
      endPos = detail::be16(p);
    }
    if (endPos <= startPos || endPos > recLen || endPos - startPos > kMaxTocEntryBytes) {
      return Error::CorruptData;
    }
    uint32_t len = endPos - startPos;
    uint8_t buf[kMaxTocEntryBytes];
    if (!readAbs(start + startPos, buf, len)) return Error::Io;
    uint32_t labelLen = buf[0];  // label length, label, tag map
    if (1 + labelLen > len) return Error::CorruptData;
    size_t n = labelLen < labelCap - 1 ? labelLen : labelCap - 1;
    memcpy(label, buf + 1, n);
    label[n] = 0;
    if (!parseTagMap(buf, len, 1 + labelLen, tt, m)) return Error::CorruptData;
    return Error::None;
  }

  Error readTocEntry(uint32_t rec, const IndxHeader& ih, uint32_t j, const TagTable& tt,
                     uint32_t ncx, const IndxHeader& mh, uint32_t index, TocEntry& te) {
    char label[8];
    TagMap m;
    Error e = readIndexEntry(rec, ih, j, tt, label, sizeof(label), m);
    if (e != Error::None) return e;

    te = TocEntry();
    te.start = Position(kNoRecord, 0);
    te.index = index;
    if (m.has[1]) te.start = textPosition(m.v[1][0]);
    if (m.has[2]) te.length = m.v[2][0];
    if (m.has[4]) te.level = static_cast<uint8_t>(m.v[4][0]);
    if (m.has[21]) te.parent = static_cast<int32_t>(m.v[21][0]);
    if (m.has[22]) te.firstChild = static_cast<int32_t>(m.v[22][0]);
    if (m.has[23]) te.lastChild = static_cast<int32_t>(m.v[23][0]);
    if (m.has[3]) ctocTitle(mh, ncx, m.v[3][0], te.title, sizeof(te.title));
    return Error::None;
  }

  // -- index cursor: sequential access to all entries of an index -------------

  /// \brief Reads all entries of an index in order, across its extra INDX records.
  struct IndxCursor {
    IndxHeader main;
    TagTable tt;
    uint32_t nextExtra = 0;   // next extra record to read
    uint32_t lastExtra = 0;   // last extra record
    uint32_t extraRec = kNoRecord;  // record of the current entries
    uint32_t j = 0;           // next entry inside that record
    IndxHeader ih;
    bool haveRecord = false;
  };

  Error indxOpen(uint32_t rec, IndxCursor& c) {
    if (rec == kNoRecord) return Error::OutOfRange;
    Error e = readIndxHeader(rec, c.main);
    if (e != Error::None) return fail(e);
    e = readTagTable(rec, c.main, c.tt);
    if (e != Error::None) return fail(e);
    c.nextExtra = rec + 1;
    c.lastExtra = rec + c.main.count;
    c.haveRecord = false;
    c.j = 0;
    return Error::None;
  }

  /// \brief Next entry of the index. Returns OutOfRange after the last entry.
  Error indxNext(IndxCursor& c, char* label, size_t labelCap, TagMap& m) {
    while (!c.haveRecord || c.j >= c.ih.count) {
      if (c.nextExtra > c.lastExtra) return Error::OutOfRange;
      Error e = readIndxHeader(c.nextExtra, c.ih);
      if (e != Error::None) return fail(e);
      c.extraRec = c.nextExtra++;
      c.j = 0;
      c.haveRecord = true;
    }
    Error e = readIndexEntry(c.extraRec, c.ih, c.j, c.tt, label, labelCap, m);
    ++c.j;
    return e == Error::None ? e : fail(e);
  }

  /// \brief Total number of entries of an index.
  Error indxTotal(uint32_t rec, uint32_t& total) {
    total = 0;
    IndxHeader h;
    Error e = readIndxHeader(rec, h);
    if (e != Error::None) return fail(e);
    for (uint32_t i = 1; i <= h.count; ++i) {
      IndxHeader ih;
      e = readIndxHeader(rec + i, ih);
      if (e != Error::None) return fail(e);
      total += ih.count;
    }
    return Error::None;
  }

  // -- KF8 parts: skeleton and fragment tables ---------------------------------

  /// \brief State of the KF8 part assembler (skeleton and fragment cursors).
  struct Kf8Gen {
    IndxCursor skel;
    IndxCursor frag;
    Error err = Error::None;
    uint32_t index = 0;        // part being assembled
    bool partOpen = false;
    bool partDone = false;
    uint32_t skelCur = 0;      // raw position in the skeleton
    uint32_t skelEnd = 0;
    uint32_t baseptr = 0;      // raw position of the next fragment text
    uint32_t fragLeft = 0;
    uint32_t partFile = 0;
    uint32_t insertedPart = 0; // bytes of fragments already inserted in this part
    bool partFileSet = false;
    bool havePending = false;  // a fragment waits to be delivered
    uint32_t pendPos = 0;
    uint32_t pendLen = 0;
  };

  static bool parseDecimal(const char* s, uint32_t& v) {
    if (*s == 0) return false;
    v = 0;
    for (; *s != 0; ++s) {
      if (*s < '0' || *s > '9' || v > 400000000u) return false;
      v = v * 10 + static_cast<uint32_t>(*s - '0');
    }
    return true;
  }

  /// \brief Compares n bytes of t with the lower case string s, ignoring case in t.
  static bool ieq(const uint8_t* t, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      uint8_t a = t[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<uint8_t>(a + 32);
      if (a != static_cast<uint8_t>(s[i])) return false;
    }
    return true;
  }

  /// \brief Opens the next skeleton (part). Returns OutOfRange after the last part.
  Error genOpenPart(Kf8Gen& g) {
    char label[64];
    TagMap m;
    Error e = indxNext(g.skel, label, sizeof(label), m);
    if (e != Error::None) return e;
    if (!m.has[1] || m.n[1] < 1 || !m.has[6] || m.n[6] < 2) {
      g.err = Error::CorruptData;
      return fail(Error::CorruptData);
    }
    g.skelCur = m.v[6][0];
    g.skelEnd = g.skelCur + m.v[6][1];
    g.baseptr = g.skelEnd;
    g.fragLeft = m.v[1][0];
    g.partOpen = true;
    g.partDone = false;
    g.partFileSet = false;
    g.partFile = 0;
    g.insertedPart = 0;
    g.havePending = false;
    return Error::None;
  }

  /// \brief Next non-empty piece of the open part, as raw text positions.
  bool genNextSegment(Kf8Gen& g, uint32_t& pos, uint32_t& len) {
    for (;;) {
      if (g.havePending) {
        g.havePending = false;
        if (g.pendLen == 0) continue;
        pos = g.pendPos;
        len = g.pendLen;
        return true;
      }
      if (!g.partOpen || g.partDone) return false;
      if (g.fragLeft > 0) {
        char label[64];
        TagMap m;
        // the fragment table gives insert positions in the assembled part, so
        // the raw skeleton position is shifted back by the fragments inserted so far
        uint32_t insertAsm = 0;
        if (indxNext(g.frag, label, sizeof(label), m) != Error::None ||
            !parseDecimal(label, insertAsm) || !m.has[3] || !m.has[6] || m.n[6] < 2 ||
            insertAsm < g.insertedPart) {
          g.err = Error::CorruptData;
          fail(Error::CorruptData);
          return false;
        }
        uint32_t insert = insertAsm - g.insertedPart;
        if (insert < g.skelCur || insert > g.skelEnd) {
          g.err = Error::CorruptData;
          fail(Error::CorruptData);
          return false;
        }
        if (!g.partFileSet) {
          g.partFile = m.v[3][0];
          g.partFileSet = true;
        }
        --g.fragLeft;
        g.havePending = true;
        g.pendPos = g.baseptr;
        g.pendLen = m.v[6][1];
        g.baseptr += m.v[6][1];
        g.insertedPart += m.v[6][1];
        pos = g.skelCur;
        len = insert - g.skelCur;
        g.skelCur = insert;
        if (len > 0) return true;
        continue;  // deliver the fragment first
      }
      // the rest of the skeleton ends the part
      g.partDone = true;
      pos = g.skelCur;
      len = g.skelEnd - g.skelCur;
      return len > 0;
    }
  }

  /// \brief Starts part 'part' (skipping the earlier parts).
  Error genStart(Kf8Gen& g, uint32_t part) {
    g = Kf8Gen();
    Error e = indxOpen(_kf8Skel, g.skel);
    if (e != Error::None) return e;
    e = indxOpen(_kf8Frag, g.frag);
    if (e != Error::None) return e;
    for (uint32_t k = 0; k < part; ++k) {
      e = genOpenPart(g);
      if (e != Error::None) return e;
      uint32_t pos = 0, len = 0;
      while (genNextSegment(g, pos, len)) {
      }
      if (g.err != Error::None) return g.err;
    }
    g.index = part;
    return genOpenPart(g);
  }

  /// \brief Finds the hybrid KF8 header: the 8 byte BOUNDARY record is followed by it.
  /// Returns kNoRecord if the book has no KF8 part.
  uint32_t findKf8Boundary() {
    auto isMarker = [&](uint32_t i) -> bool {
      if (i >= _numRecords) return false;
      uint32_t s = recordOffset(i), e = recordEnd(i);
      if (s == kNoRecord || e == kNoRecord || e < s || e - s != 8) return false;
      uint8_t b[8];
      return readAbs(s, b, sizeof(b)) && memcmp(b, "BOUNDARY", 8) == 0;
    };
    if (_info.kf8Boundary != kNoRecord && isMarker(_info.kf8Boundary)) {
      return _info.kf8Boundary + 1;
    }
    for (uint32_t i = 0; i + 1 < _numRecords; ++i) {
      if (isMarker(i)) return i + 1;
    }
    return kNoRecord;
  }

  /// \brief Reads the KF8 header and sets up the KF8 text view and indexes.
  Error ensureKf8() {
    if (!WithKf8) return fail(Error::Disabled);
    if (_kf8Loaded) return Error::None;
    if (!_open) return fail(Error::NotOpen);
    uint32_t hdr = 0;
    if (!_info.isKf8) {
      hdr = findKf8Boundary();
      if (hdr == kNoRecord) return fail(Error::NotMobi);
    }
    uint32_t start = recordOffset(hdr), end = recordEnd(hdr);
    if (start == kNoRecord || end == kNoRecord || end > _total || end < start ||
        end - start < 0x100) {
      return fail(Error::CorruptData);
    }
    uint8_t h[0x100];
    if (!readAbs(start, h, sizeof(h))) return fail(Error::Io);
    if (memcmp(h + 16, "MOBI", 4) != 0) return fail(Error::NotMobi);
    if (detail::be16(h + 12) != 0) return fail(Error::Encrypted);
    uint16_t comp = detail::be16(h);
    if (comp != 1 && comp != 2) return fail(Error::UnsupportedCompression);
    uint32_t headerLen = detail::be32(h + 20);
    uint32_t recCount = detail::be16(h + 8);
    uint32_t extra = headerLen + 16 >= 0xF4 ? detail::be16(h + 0xF2) : 0;

    auto absIndex = [&](uint32_t rel) -> uint32_t {
      if (headerLen + 16 < rel + 4) return kNoRecord;
      uint32_t v = detail::be32(h + rel);
      if (v == kNoRecord || hdr + v >= _numRecords) return kNoRecord;
      return hdr + v;
    };
    _kf8Frag = absIndex(0xF8);
    _kf8Skel = absIndex(0xFC);

    _kf8View = TextView();
    _kf8View.firstRec = hdr + 1;
    uint32_t avail = _numRecords - (hdr + 1);
    _kf8View.chunks = recCount < avail ? recCount : avail;
    _kf8View.recordSize = detail::be16(h + 10);
    _kf8View.textLength = detail::be32(h + 4);
    _kf8View.compression = comp;
    _kf8View.extraFlags = static_cast<uint16_t>(extra);
    _rawChunk = kNoRecord;
    _kf8Loaded = true;
    _kf8Parts = 0;
    if (_kf8Skel != kNoRecord) {
      uint32_t parts = 0;
      if (indxTotal(_kf8Skel, parts) == Error::None) _kf8Parts = parts;
    }
    return Error::None;
  }

  /// \brief Copies n bytes of the KF8 source text starting at raw position pos.
  size_t rawCopy(uint32_t pos, uint8_t* dst, size_t n) {
    if (!WithKf8) return 0;
    const uint32_t rs = _kf8View.recordSize;
    if (rs == 0) {
      fail(Error::CorruptData);
      return 0;
    }
    size_t done = 0;
    while (done < n) {
      uint32_t off = pos + static_cast<uint32_t>(done);
      uint32_t chunk = off / rs;
      uint32_t inOff = off - chunk * rs;
      if (chunk >= _kf8View.chunks) break;
      if (_rawChunk != chunk) {
        size_t len = 0;
        Error e = decodeChunkIn(_kf8View, chunk, _raw, sizeof(_raw), len);
        if (e != Error::None) {
          fail(e);
          break;
        }
        _rawChunk = chunk;
        _rawLen = static_cast<uint32_t>(len);
      }
      if (inOff >= _rawLen) break;
      size_t take = _rawLen - inOff;
      if (take > n - done) take = n - done;
      memcpy(dst + done, _raw + inOff, take);
      done += take;
    }
    return done;
  }

  /// \brief Fills the text buffer with the next piece of the assembled KF8 part.
  bool kfFill() {
    if (!_firstFill) _secBase += _recLen;
    _firstFill = false;
    size_t filled = 0;
    while (filled < RecordBufferSize) {
      if (_kfSegLeft == 0) {
        uint32_t pos = 0, len = 0;
        if (!genNextSegment(_gen, pos, len)) break;
        _kfSegPos = pos;
        _kfSegLeft = len;
      }
      size_t want = RecordBufferSize - filled;
      if (want > _kfSegLeft) want = _kfSegLeft;
      size_t got = rawCopy(_kfSegPos, _rec + filled, want);
      if (got == 0) {
        fail(Error::CorruptData);
        break;
      }
      _kfSegPos += static_cast<uint32_t>(got);
      _kfSegLeft -= static_cast<uint32_t>(got);
      filled += got;
    }
    if (filled == 0) {
      _recLen = _recPos = 0;
      return false;
    }
    _recLen = static_cast<uint32_t>(filled);
    _recPos = 0;
    return true;
  }

  /// \brief Parses an image tag. Returns the 1 based image number of recindex or
  /// kindle:embed (base 32) in value.
  static bool parseImageTag(const uint8_t* t, size_t n, uint32_t& value) {
    if (n < 3 || !ieq(t, "img", 3)) return false;
    if (n > 3 && t[3] != ' ' && t[3] != '\t' && t[3] != '\r' && t[3] != '\n' && t[3] != '/') {
      return false;
    }
    for (size_t i = 3; i + 8 <= n; ++i) {
      if (!ieq(t + i, "recindex", 8)) continue;
      size_t k = i + 8;
      while (k < n && t[k] == ' ') ++k;
      if (k >= n || t[k] != '=') return false;
      ++k;
      while (k < n && t[k] == ' ') ++k;
      if (k < n && (t[k] == '"' || t[k] == '\'')) ++k;
      uint32_t v = 0;
      size_t digits = 0;
      for (; k < n && t[k] >= '0' && t[k] <= '9' && v < 100000000u; ++k, ++digits) {
        v = v * 10 + (t[k] - '0');
      }
      if (digits == 0) return false;
      value = v;
      return true;
    }
    for (size_t i = 3; i + 13 <= n; ++i) {
      if (!ieq(t + i, "kindle:embed:", 13)) continue;
      size_t k = i + 13;
      uint32_t v = 0;
      size_t digits = 0;
      for (; k < n && v < 0x3FFFFFF; ++k) {
        uint8_t c = t[k];
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'V') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'v') d = c - 'a' + 10;
        else break;
        v = v * 32 + d;
        ++digits;
      }
      if (digits == 0) return false;
      value = v;
      return true;
    }
    return false;
  }

  Error fail(Error e) {
    _error = e;
    return e;
  }

  bool readRel(uint32_t rel, void* dst, size_t n) {
    if (rel > _rec0Len || n > _rec0Len - rel) return false;
    return _src.read(_rec0 + rel, dst, n) == n;
  }

  bool rd16(uint32_t rel, uint32_t& v) {
    uint8_t b[2];
    if (!readRel(rel, b, 2)) return false;
    v = detail::be16(b);
    return true;
  }

  bool rd32(uint32_t rel, uint32_t& v) {
    uint8_t b[4];
    if (!readRel(rel, b, 4)) return false;
    v = detail::be32(b);
    return true;
  }

  /// \brief Reads a 32 bit MOBI header field if the header is long enough.
  bool mobiField(uint32_t rel, uint32_t& v) {
    if (rel + 4 > 16 + _mobiHeaderLen) return false;
    return rd32(rel, v);
  }

  // -- PalmDB record table -------------------------------------------------

  uint32_t recordOffset(uint32_t i) {
    if (i >= _numRecords) return kNoRecord;
    uint8_t b[4];
    if (_src.read(78 + 8 * i, b, 4) != 4) return kNoRecord;
    return detail::be32(b);
  }

  uint32_t recordEnd(uint32_t i) {
    if (i + 1 >= _numRecords) return _total;
    return recordOffset(i + 1);
  }

  // -- metadata ------------------------------------------------------------

  /// \brief Maps an image number (1 based, as used by recindex and kindle:embed)
  /// to its PalmDB record. Returns kNoRecord if there is no such image.
  uint32_t imageRecordOf(uint32_t number) const {
    if (!_open || number == 0 || _info.firstImageRecord == kNoRecord) return kNoRecord;
    uint32_t idx = _info.firstImageRecord + number - 1;
    return idx < _numRecords ? idx : kNoRecord;
  }

  /// \brief Number of sections of the book text: KF8 parts or MOBI6 text records.
  uint32_t sectionCount() const { return _useKf8 ? _kf8Parts : _mobi6View.chunks; }

  /// \brief Section position of an offset in the MOBI6 text (TOC entries).
  /// Returns Position(kNoRecord, 0) if it cannot be resolved, e.g. in KF8 books.
  /// \brief Decodes a base 32 number of KF8 links: digits and the letters A to V.
  static bool base32(const char* s, size_t n, uint32_t& v) {
    if (n == 0 || n > 10) return false;
    uint64_t acc = 0;
    for (size_t i = 0; i < n; ++i) {
      const char c = s[i];
      uint32_t d;
      if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
      else if (c >= 'A' && c <= 'V') d = static_cast<uint32_t>(c - 'A' + 10);
      else return false;
      acc = acc * 32 + d;
      if (acc > 0xFFFFFFFFull) return false;
    }
    v = static_cast<uint32_t>(acc);
    return true;
  }

  /// \brief The raw start position of skeleton part 'part' (its tag 6 value).
  bool skeletonStart(uint32_t part, uint32_t& start) {
    IndxCursor sc;
    if (indxOpen(_kf8Skel, sc) != Error::None) return false;
    char label[64];
    TagMap tm;
    for (uint32_t i = 0; indxNext(sc, label, sizeof(label), tm) == Error::None; ++i) {
      if (i != part) continue;
      if (!tm.has[6] || tm.n[6] < 1) return false;
      start = tm.v[6][0];
      return true;
    }
    return false;
  }

  /// \brief Resolves "kindle:pos:fid:XXXX:off:YYYYYYYYYY" (KF8 and hybrid books).
  /// fid is the index of a fragment; its insert position in the assembled part plus
  /// off is the offset in the part, and the fragment gives the part (file number).
  bool resolveKf8Link(const char* t, Position& out) {
    static const char kFid[] = "kindle:pos:fid:";
    static const char kOff[] = ":off:";
    const size_t fl = sizeof(kFid) - 1, ol = sizeof(kOff) - 1;
    if (!_useKf8 || strncmp(t, kFid, fl) != 0) return false;
    const char* f = t + fl;
    size_t k = 0;
    while (f[k] != 0 && f[k] != ':') ++k;
    uint32_t fid = 0;
    if (!base32(f, k, fid)) return false;
    const char* o = f + k;
    if (strncmp(o, kOff, ol) != 0) return false;
    o += ol;
    size_t m = 0;
    while (o[m] != 0) ++m;
    uint32_t off = 0;
    if (!base32(o, m, off)) return false;

    // the fragment with index fid, found by walking the fragment index
    IndxCursor c;
    if (indxOpen(_kf8Frag, c) != Error::None) return false;
    char label[64];
    TagMap tm;
    for (uint32_t i = 0; indxNext(c, label, sizeof(label), tm) == Error::None; ++i) {
      if (i != fid) continue;
      uint32_t insert = 0;
      if (!parseDecimal(label, insert) || !tm.has[3] || tm.n[3] < 1) return false;
      const uint32_t part = tm.v[3][0];
      if (part >= sectionCount()) return false;
      // the insert position is a raw position; the part starts at the skeleton's start
      uint32_t skelStart = 0;
      if (!skeletonStart(part, skelStart) || insert < skelStart) return false;
      const uint64_t pos = static_cast<uint64_t>(insert - skelStart) + off;
      if (pos > 0xFFFFFFFFull) return false;
      out = Position(part, static_cast<uint32_t>(pos));
      return true;
    }
    return false;
  }

  Position textPosition(uint32_t textOffset) {
    if (!_open || _useKf8 || _mobi6View.recordSize == 0) return Position(kNoRecord, 0);
    if (_mobi6View.compression != kHuffCompression) {
      uint32_t sec = textOffset / _mobi6View.recordSize;
      if (sec >= _mobi6View.chunks) return Position(kNoRecord, 0);
      return Position(sec, textOffset - sec * _mobi6View.recordSize);
    }
    // HUFF books: record sizes vary, so the walk adds up the decoded lengths.
    // The walk remembers where it stopped, so sorted lookups stay linear.
    if (textOffset < _walkStart) {
      _walkSection = 0;
      _walkStart = 0;
    }
    uint32_t savedSection = _curChunk, savedPos = _recPos;
    bool hadChunk = _recLen > 0;
    Position result(kNoRecord, 0);
    while (_walkSection < _mobi6View.chunks) {
      size_t n = 0;
      if (decodeChunkIn(_mobi6View, _walkSection, _rec, RecordBufferSize, n) != Error::None) break;
      if (textOffset < _walkStart + n) {
        result = Position(_walkSection, textOffset - _walkStart);
        break;
      }
      _walkStart += static_cast<uint32_t>(n);
      ++_walkSection;
    }
    if (hadChunk) {  // restore the reading position
      _nextChunk = savedSection;
      loadNext();
      _recPos = savedPos < _recLen ? savedPos : _recLen;
    } else {
      _recLen = _recPos = 0;
    }
    return result;
  }

  /// \brief Copies len bytes at absolute position pos as UTF-8 into dst (truncated).
  void copyText(uint32_t pos, uint32_t len, char* dst, size_t cap) {
    dst[0] = 0;
    if (cap < 2) return;
    size_t cap1 = cap - 1;
    size_t o = 0;
    uint32_t done = 0;
    uint8_t buf[32];
    bool full = false;
    while (done < len && !full) {
      size_t want = len - done < sizeof(buf) ? static_cast<size_t>(len - done) : sizeof(buf);
      size_t n = _src.read(pos + done, buf, want);
      if (n == 0) break;
      size_t k = 0;
      for (; k < n; ++k) {
        uint8_t b = buf[k];
        bool ascii = _utf8 || b < 0x80;
        uint32_t cp = ascii ? b : detail::cp1252ToUnicode(b);
        size_t need = ascii ? 1 : detail::utf8Len(cp);
        if (o + need > cap1) {
          full = true;
          break;
        }
        if (ascii) dst[o++] = static_cast<char>(b);
        else o += detail::utf8Encode(cp, reinterpret_cast<uint8_t*>(dst) + o);
      }
      done += static_cast<uint32_t>(k);
    }
    o = detail::trimIncompleteUtf8(dst, o);
    dst[o] = 0;
  }

  void parseExth() {
    uint8_t magic[4];
    if (!readRel(_mobiHeaderLen + 16, magic, 4) || memcmp(magic, "EXTH", 4) != 0) return;
    uint32_t count = 0;
    if (!rd32(_mobiHeaderLen + 16 + 8, count)) return;

    uint32_t coverOffset = kNoRecord;
    uint32_t p = _mobiHeaderLen + 16 + 12;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t type = 0, len = 0;
      if (!rd32(p, type) || !rd32(p + 4, len)) break;
      if (len < 8 || len > _rec0Len - p) break;
      uint32_t dataRel = p + 8;
      uint32_t dataLen = len - 8;
      switch (type) {
        case 100: copyText(_rec0 + dataRel, dataLen, _meta.author, sizeof(_meta.author)); break;
        case 101: copyText(_rec0 + dataRel, dataLen, _meta.publisher, sizeof(_meta.publisher)); break;
        case 106: copyText(_rec0 + dataRel, dataLen, _meta.published, sizeof(_meta.published)); break;
        case 524: copyText(_rec0 + dataRel, dataLen, _meta.language, sizeof(_meta.language)); break;
        case 503:  // updated title overrides the full name
          if (dataLen > 0) copyText(_rec0 + dataRel, dataLen, _meta.title, sizeof(_meta.title));
          break;
        case 121: {  // KF8 boundary record
          uint32_t v = 0;
          if (dataLen >= 4 && rd32(dataRel, v) && v < _numRecords) _info.kf8Boundary = v;
          break;
        }
        case 201: {  // cover: index relative to the first image record
          uint32_t v = 0;
          if (dataLen >= 4 && rd32(dataRel, v)) coverOffset = v;
          break;
        }
        default: break;
      }
      p += len;
    }
    if (coverOffset != kNoRecord && _info.firstImageRecord != kNoRecord) {
      uint32_t idx = _info.firstImageRecord + coverOffset;
      if (idx < _numRecords) _info.coverRecord = idx;
    }
  }

  // -- text decoding -------------------------------------------------------

  /// \brief Reads the chunk's compressed bytes via the source and decompresses them.
  Error decodeChunkIn(const TextView& v, uint32_t chunk, uint8_t* out, size_t cap,
                      size_t& outLen) {
    outLen = 0;
    uint32_t start = recordOffset(v.firstRec + chunk);
    uint32_t end = recordEnd(v.firstRec + chunk);
    if (start == kNoRecord || end == kNoRecord || end > _total || end < start) {
      return fail(Error::CorruptData);
    }
    uint32_t len = end - start;
    if (len == 0) return Error::None;

    uint32_t trail = 0;
    if (v.extraFlags != 0 && !trailingSizeAt(start, len, v.extraFlags, trail)) {
      return fail(Error::CorruptData);
    }
    if (trail > len) return fail(Error::CorruptData);
    uint32_t dataLen = len - trail;

    if (v.compression == 1) {
      if (dataLen > cap) return fail(Error::BufferTooSmall);
      if (dataLen > 0 && _src.read(start, out, dataLen) != dataLen) return fail(Error::Io);
      outLen = dataLen;
      return Error::None;
    }
    if (v.compression == kHuffCompression) {
      size_t o = 0;
      Error e = huffUnpack(start, dataLen, out, cap, o, 0);
      outLen = o;
      return e;
    }
    return palmdocDecompress(start, dataLen, out, cap, outLen);
  }


  /// \brief Byte cache for reading record tails backwards from the source.
  /// Cache of source bytes for reading record tails backwards.
  struct ByteWindow {
    uint32_t base = 0;
    uint32_t len = 0;
    uint8_t buf[16];
  };

  /// \brief Byte fromEnd positions before the end of the record [start, start+len).
  bool byteFromEnd(ByteWindow& w, uint32_t start, uint32_t len, uint32_t fromEnd, uint8_t& b) {
    if (fromEnd == 0 || fromEnd > len) return false;
    uint32_t abs = start + len - fromEnd;
    if (abs < w.base || abs >= w.base + w.len) {
      uint32_t lo = abs >= 15 ? abs - 15 : 0;
      if (lo < start) lo = start;
      uint32_t hi = start + len;
      uint32_t n = hi - lo < sizeof(w.buf) ? hi - lo : static_cast<uint32_t>(sizeof(w.buf));
      if (!readAbs(lo, w.buf, n)) return false;
      w.base = lo;
      w.len = n;
    }
    b = w.buf[abs - w.base];
    return true;
  }

  /// \brief Computes the number of trailing bytes of a record (entries and the
  /// multibyte overlap descriptor). Entries are read backwards from the end.
  bool trailingSizeAt(uint32_t start, uint32_t len, uint16_t flags, uint32_t& total) {
    ByteWindow w;
    uint32_t num = 0;
    uint16_t f = static_cast<uint16_t>(flags >> 1);
    for (; f != 0; f = static_cast<uint16_t>(f >> 1)) {
      if ((f & 1) == 0) continue;
      // backward encoded variable length integer; value includes itself
      uint32_t value = 0, shift = 0, i = 0;
      for (;;) {
        uint8_t v = 0;
        if (!byteFromEnd(w, start, len, num + i + 1, v)) return false;
        ++i;
        value |= static_cast<uint32_t>(v & 0x7F) << shift;
        shift += 7;
        if ((v & 0x80) != 0 || shift >= 28) break;
      }
      if (value == 0 || value > len - num) return false;
      num += value;
    }
    if ((flags & 1) != 0) {  // multibyte overlap descriptor
      uint8_t d = 0;
      if (!byteFromEnd(w, start, len, num + 1, d)) return false;
      num += static_cast<uint32_t>((d & 0x3) + 1);
    }
    total = num;
    return num <= len;
  }

  /// \brief PalmDOC LZ77 decompression. Reads the input in small blocks, so no
  /// additional memory is needed beyond the output buffer.
  Error palmdocDecompress(uint32_t start, uint32_t inLen, uint8_t* out, size_t cap,
                          size_t& outLen) {
    uint8_t in[32];
    uint32_t inPos = 0;     // bytes fetched from the source
    uint32_t inBufLen = 0;  // valid bytes in in[]
    uint32_t inBufPos = 0;
    bool ioError = false;

    auto nextByte = [&](uint8_t& b) -> bool {
      if (inBufPos >= inBufLen) {
        if (inPos >= inLen) return false;
        uint32_t want = inLen - inPos < sizeof(in) ? inLen - inPos : sizeof(in);
        if (_src.read(start + inPos, in, want) != want) {
          ioError = true;
          return false;
        }
        inPos += want;
        inBufLen = want;
        inBufPos = 0;
      }
      b = in[inBufPos++];
      return true;
    };

    size_t o = 0;
    uint8_t c = 0;
    while (nextByte(c)) {
      if (c == 0 || (c >= 0x09 && c <= 0x7F)) {  // literal
        if (o >= cap) return fail(Error::BufferTooSmall);
        out[o++] = c;
      } else if (c <= 0x08) {  // the next c bytes are literal
        for (uint8_t k = 0; k < c; ++k) {
          uint8_t b = 0;
          if (!nextByte(b)) return fail(ioError ? Error::Io : Error::CorruptData);
          if (o >= cap) return fail(Error::BufferTooSmall);
          out[o++] = b;
        }
      } else if (c <= 0xBF) {  // back reference: 11 bit distance, 3 bit length
        uint8_t c2 = 0;
        if (!nextByte(c2)) return fail(ioError ? Error::Io : Error::CorruptData);
        uint32_t code = ((static_cast<uint32_t>(c) << 8) | c2) & 0x3FFF;
        size_t dist = code >> 3;
        size_t len = (code & 0x7) + 3;
        if (dist == 0 || dist > o) return fail(Error::CorruptData);
        if (o + len > cap) return fail(Error::BufferTooSmall);
        for (size_t k = 0; k < len; ++k, ++o) out[o] = out[o - dist];
      } else {  // 0xC0..0xFF: a space followed by a character
        if (o + 2 > cap) return fail(Error::BufferTooSmall);
        out[o++] = ' ';
        out[o++] = static_cast<uint8_t>(c ^ 0x80);
      }
    }
    if (ioError) return fail(Error::Io);
    outLen = o;
    return Error::None;
  }

  /// \brief Loads the next text chunk (MOBI6) or delivers the next piece of the
  /// current KF8 part, opening the next part when needed.
  bool loadNext() {
    if (_useKf8) return loadKf8Part();
    if (_nextChunk >= _mobi6View.chunks) {
      _recLen = _recPos = 0;
      return false;
    }
    size_t n = 0;
    Error e = decodeChunkIn(_mobi6View, _nextChunk, _rec, RecordBufferSize, n);
    if (e != Error::None) {
      _recLen = _recPos = 0;
      return false;
    }
    _curChunk = _nextChunk++;
    _secBase = 0;
    _recLen = static_cast<uint32_t>(n);
    _recPos = 0;
    return true;
  }

  /// \brief KF8 text: the parts are read one after the other.
  bool loadKf8Part() {
    for (;;) {
      if (_partOpen && kfFill()) return true;
      _partOpen = false;
      if (_nextChunk >= _kf8Parts) {
        _recLen = _recPos = 0;
        return false;
      }
      uint32_t part = _nextChunk;
      Error e;
      if (_genStarted && _gen.index + 1 == part) {
        e = genOpenPart(_gen);
        _gen.index = part;
      } else {
        e = genStart(_gen, part);
      }
      if (e != Error::None) {
        _recLen = _recPos = 0;
        return false;
      }
      _genStarted = true;
      _partOpen = true;
      _kfSegLeft = 0;
      _firstFill = true;
      _secBase = 0;
      _curChunk = part;
      _nextChunk = part + 1;
      _recLen = _recPos = 0;
    }
  }

  // -- output stage --------------------------------------------------------

  enum : uint32_t { kPendSize = 160, kAttrSize = 128 };

  void emitPending(const uint8_t* bytes, size_t n) {
    memcpy(_pend, bytes, n);
    _pendLen = static_cast<uint8_t>(n);
    _pendOff = 0;
    _lastOut = bytes[n - 1];
  }

  /// \brief Emits an image marker: an empty line, kImageBegin, the record number,
  /// kImageEnd and a newline (the image is a paragraph of its own).
  void emitImageMarker(uint32_t record) {
    uint8_t buf[kPendSize];
    size_t n = 0;
    if (_lastOut != '\n') buf[n++] = '\n';
    buf[n++] = '\n';
    buf[n++] = kImageBegin;
    char digits[10];
    int k = 0;
    do {
      digits[k++] = static_cast<char>('0' + record % 10);
      record /= 10;
    } while (record != 0);
    while (k > 0) buf[n++] = static_cast<uint8_t>(digits[--k]);
    buf[n++] = kImageEnd;
    buf[n++] = '\n';
    emitPending(buf, n);
  }

  /// \brief Emits a link marker: kLinkBegin, target, kLinkTarget (the link text follows).
  void emitLinkBegin(const char* target, size_t n) {
    uint8_t buf[kPendSize];
    buf[0] = kLinkBegin;
    memcpy(buf + 1, target, n);
    buf[1 + n] = kLinkTarget;
    emitPending(buf, n + 2);
  }

  void emitByte(uint8_t b) { emitPending(&b, 1); }

  void emitCodepoint(uint32_t cp) {
    uint8_t buf[4];
    size_t n = detail::utf8Encode(cp, buf);
    emitPending(buf, n);
  }

  /// \brief Emits one raw book byte (CP1252 converted to UTF-8 if needed).
  void emitRaw(uint8_t b) {
    if (_utf8 || b < 0x80) emitByte(b);
    else emitCodepoint(detail::cp1252ToUnicode(b));
  }

  /// Compares the name of the last tag with s (lower case).
  bool tagNameIs(const char* s) const {
    const size_t n = strlen(s);
    return n == _tagNameLen && memcmp(s, _tagName, n) == 0;
  }

  bool isHeadingTag() const {
    return _tagNameLen == 2 && _tagName[0] == 'h' && _tagName[1] >= '1' && _tagName[1] <= '6';
  }

  bool isBlockTag() const {
    static const char* const kBlock[] = {"p",  "div", "br", "li", "tr", "ul",
                                         "ol", "h1",  "h2", "h3", "h4", "h5",
                                         "h6", "blockquote", "table"};
    for (const char* t : kBlock) {
      size_t n = strlen(t);
      if (n == _tagNameLen && memcmp(t, _tagName, n) == 0) return true;
    }
    return false;
  }

  /// \brief Finds the link target in the attributes of an <a> tag: "filepos=<digits>"
  /// (MOBI6) or the value of a "kindle:pos:..." href (KF8). Returns false otherwise.
  static bool linkTargetIn(const char* a, size_t n, char* out, size_t& len) {
    static const char kFilepos[] = "filepos=";
    static const char kKindle[] = "kindle:pos:";
    const size_t fl = sizeof(kFilepos) - 1, kl = sizeof(kKindle) - 1;
    for (size_t i = 0; i < n; ++i) {
      if (i + fl <= n && memcmp(a + i, kFilepos, fl) == 0) {
        size_t j = i + fl, d = 0;
        while (j < n && a[j] >= '0' && a[j] <= '9' && d < 12) {
          ++j;
          ++d;
        }
        if (d == 0) return false;
        memcpy(out, kFilepos, fl);
        memcpy(out + fl, a + i + fl, d);
        len = fl + d;
        return true;
      }
      if (i + kl <= n && memcmp(a + i, kKindle, kl) == 0) {
        size_t j = i;
        while (j < n && j - i < kAttrSize - 1 && a[j] != '"' && a[j] != '\'' && a[j] != ' ' && a[j] != '>') ++j;
        memcpy(out, a + i, j - i);
        len = j - i;
        return true;
      }
    }
    return false;
  }

  /// \brief Plain mode: decodes "&name;" / "&#nnn;" that lies inside the chunk.
  bool decodeEntity() {
    uint32_t limit = _recPos + 11 < _recLen ? _recPos + 11 : _recLen;
    uint32_t k = _recPos;
    while (k < limit && _rec[k] != ';') ++k;
    if (k >= limit) return false;
    uint32_t cp = 0;
    if (!detail::lookupEntity(_rec + _recPos, k - _recPos, cp)) return false;
    _recPos = k + 1;
    emitCodepoint(cp);
    return true;
  }

  /// \brief Produces the next output unit into the pending buffer.
  /// Returns false at the end of the text.
  bool produce() {
    for (;;) {
      while (_recPos >= _recLen) {
        if (!loadNext()) return false;
      }
      uint8_t b = _rec[_recPos++];
      if (_mode == TextMode::Markup) {
        emitRaw(b);
        return true;
      }

      // plain text mode
      if (_inTag) {
        if (b == '>') {
          _inTag = false;
          // <head> and <title> hold metadata, not page text: their content is skipped
          if (_skipping) {
            // the closing tag ends the skip; <body> ends it too, in case the head is not closed
            const bool closes = _tagClose && _tagNameLen == _skipNameLen &&
                                memcmp(_tagName, _skipName, _skipNameLen) == 0;
            if (closes || (!_tagClose && tagNameIs("body"))) _skipping = false;
          } else if (!_tagClose && (tagNameIs("head") || tagNameIs("title"))) {
            _skipping = true;
            memcpy(_skipName, _tagName, _tagNameLen);
            _skipNameLen = _tagNameLen;
          }
          if (_mode == TextMode::PlainLinks && !_skipping && tagNameIs("a")) {
            if (!_tagClose) {
              char target[kAttrSize];
              size_t tl = 0;
              if (linkTargetIn(_attr, _attrLen, target, tl)) {
                _linkOpen = true;
                emitLinkBegin(target, tl);
                return true;
              }
            } else if (_linkOpen) {
              _linkOpen = false;
              emitByte(kLinkEnd);
              return true;
            }
          }
          if (_mode == TextMode::PlainLinks && !_skipping && !_tagClose && tagNameIs("img")) {
            // an image: its own paragraph with the PalmDB record of the image
            // "img" and a space (the attributes do not keep the space after the name), then the attributes
            char tagBuf[4 + kAttrSize];
            memcpy(tagBuf, "img ", 4);
            memcpy(tagBuf + 4, _attr, _attrLen);
            uint32_t number = 0;
            if (parseImageTag(reinterpret_cast<const uint8_t*>(tagBuf), 4 + _attrLen, number)) {
              const uint32_t record = imageRecordOf(number);
              if (record != kNoRecord) {
                emitImageMarker(record);
                return true;
              }
            }
          }
          if (_mode == TextMode::PlainLinks && !_skipping && isHeadingTag()) {
            // a heading is a paragraph of its own, after an empty line: the newlines
            // end the paragraph before it and make the empty one (marker, then the
            // closing marker and a newline)
            uint8_t buf[3];
            size_t n = 0;
            if (!_tagClose) {
              buf[n++] = '\n';
              if (_lastOut != '\n') buf[n++] = '\n';
              buf[n++] = kHeadBegin;
            } else {
              buf[n++] = kHeadEnd;
              buf[n++] = '\n';
            }
            emitPending(buf, n);
            return true;
          }
          if (!_skipping && isBlockTag() && _lastOut != '\n') {
            emitByte('\n');
            return true;
          }
        } else if (!_tagNameDone) {
          // letters, and digits after the first letter (h1 .. h6)
          const bool nameChar = detail::isAsciiAlpha(b) || (_tagNameLen > 0 && b >= '0' && b <= '9');
          if (nameChar && _tagNameLen < sizeof(_tagName)) {
            _tagName[_tagNameLen++] = detail::lowerAscii(b);
          } else if (b == '/' && _tagNameLen == 0) {
            _tagClose = true;
          } else {
            _tagNameDone = true;
          }
        } else if (_mode == TextMode::PlainLinks && _attrLen < sizeof(_attr)) {
          _attr[_attrLen++] = static_cast<char>(b);
        }
        continue;
      }
      if (b == '<') {
        _inTag = true;
        _tagNameLen = 0;
        _tagNameDone = false;
        _tagClose = false;
        _attrLen = 0;
        continue;
      }
      if (_skipping) continue;
      if (b == '&') {
        if (decodeEntity()) return true;
        emitByte('&');
        return true;
      }
      if (b == ' ' || b == '\n' || b == '\r' || b == '\t') {  // collapse white space
        if (_lastOut == ' ' || _lastOut == '\n') continue;
        emitByte(' ');
        return true;
      }
      emitRaw(b);
      return true;
    }
  }

  // -- members -------------------------------------------------------------

  Source _src;
  TextMode _mode;
  Error _error = Error::None;
  bool _open = false;
  bool _utf8 = false;

  // HUFF/CDIC tables (only used for compression 17480)
  uint32_t _dict1[256] = {0};  // 8 bit prefix -> code length, flags, max code
  uint32_t _dict2[64] = {0};   // per code length: min and max code
  uint32_t _phrases = 0;       // number of dictionary phrases
  uint32_t _cdicFirst = 0;     // record index of the first CDIC record
  uint32_t _cdicCount = 0;     // number of CDIC records
  uint32_t _cdicBits = 0;      // phrases per CDIC record = 2^bits

  // text views: MOBI6 (set in open) and KF8 (set by ensureKf8)
  TextView _mobi6View;
  TextView _kf8View;
  bool _kf8Loaded = false;
  uint32_t _kf8Skel = kNoRecord;  // skeleton index record
  uint32_t _kf8Frag = kNoRecord;  // fragment index record
  Kf8Gen _gen;
  uint32_t _kfSegPos = 0;
  uint32_t _kfSegLeft = 0;
  bool _useKf8 = false;           // the text is the KF8 text (assembled parts)
  bool _partOpen = false;         // a KF8 part is being delivered
  bool _firstFill = true;         // first piece of the current part
  bool _genStarted = false;       // the part generator has been started
  uint32_t _kf8Parts = 0;         // number of KF8 parts
  uint32_t _secBase = 0;          // bytes of the current section before _rec
  uint32_t _walkSection = 0;      // HUFF position walk: next section
  uint32_t _walkStart = 0;        // HUFF position walk: text offset of that section
  uint8_t _raw[WithKf8 ? RecordBufferSize : 1]; // decompressed KF8 source chunk
  uint32_t _rawChunk = kNoRecord;
  uint32_t _rawLen = 0;

  Info _info;
  Metadata _meta;

  uint32_t _total = 0;
  uint32_t _numRecords = 0;
  uint32_t _rec0 = 0;
  uint32_t _rec0Len = 0;
  uint32_t _mobiHeaderLen = 0;

  // text stream state
  uint8_t _rec[RecordBufferSize];   // decompressed current chunk
  uint32_t _recLen = 0;
  uint32_t _recPos = 0;
  uint32_t _curChunk = 0;
  uint32_t _nextChunk = 0;

  uint8_t _pend[kPendSize];         // output not yet returned by read()
  uint8_t _pendLen = 0;
  uint8_t _pendOff = 0;
  uint8_t _lastOut = '\n';

  bool _inTag = false;              // plain mode tag state
  bool _tagNameDone = false;
  bool _tagClose = false;           // the current tag is a closing tag
  uint8_t _tagNameLen = 0;
  char _tagName[12];
  bool _skipping = false;           // inside <head> or <title>: no output
  char _skipName[12];               // name of the element being skipped
  uint8_t _skipNameLen = 0;
  char _attr[kAttrSize];           // attributes of the current tag (PlainLinks)
  uint8_t _attrLen = 0;
  bool _linkOpen = false;           // a link marker was emitted and its end is not yet
};

/// \brief Convenience aliases.
using MemoryMobi = MobiDocument<MemorySource>;

/// \brief Same as MemoryMobi without KF8 support: saves one record buffer of RAM.
using MemoryMobiNoKf8 = MobiDocument<MemorySource, 4096, false>;

template <typename File>
using FileMobi = MobiDocument<StreamSource<File>>;

}  // namespace tinymobi

// Makes the tinymobi namespace visible, so the classes can be used without the
// prefix (MemoryMobi, FileMobi, Error, ...). Define TINYMOBI_NO_USING_NAMESPACE
// before including this header to avoid it (then write tinymobi::MemoryMobi).
#ifndef TINYMOBI_NO_USING_NAMESPACE
using namespace tinymobi;
#endif
