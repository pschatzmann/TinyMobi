// Host tests for TinyMobi.
// Build: g++ -std=c++11 -Wall -Wextra -pedantic -fsanitize=address,undefined -I src tests/test_tinymobi.cpp -o /tmp/t
// Run:   ./t [file.mobi ...]   (extra files are parsed and streamed as smoke tests)

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "TinyMobi.h"

using namespace tinymobi;

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      ++g_failures;                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
    }                                                                  \
  } while (0)

// ---------------------------------------------------------------------------
// Synthetic MOBI builder
// ---------------------------------------------------------------------------

typedef std::vector<uint8_t> Bytes;

static void put16(Bytes& b, size_t pos, uint32_t v) {
  b[pos] = uint8_t(v >> 8);
  b[pos + 1] = uint8_t(v);
}
static void put32(Bytes& b, size_t pos, uint32_t v) {
  b[pos] = uint8_t(v >> 24);
  b[pos + 1] = uint8_t(v >> 16);
  b[pos + 2] = uint8_t(v >> 8);
  b[pos + 3] = uint8_t(v);
}
static void append(Bytes& dst, const std::string& s) { dst.insert(dst.end(), s.begin(), s.end()); }
static void append(Bytes& dst, const Bytes& s) { dst.insert(dst.end(), s.begin(), s.end()); }

struct BookSpec {
  uint16_t compression = 2;     // 1 or 2
  uint32_t encoding = 65001;    // 65001 or 1252
  uint32_t encryption = 0;
  uint16_t extraFlags = 0;
  std::string title = "Synthetic Title";
  std::string author = "Ada Lovelace";
  std::vector<Bytes> textRecords;  // already compressed / trailing-decorated
  Bytes image;                     // optional extra record (raw bytes)
  std::vector<Bytes> extraRecords; // appended after the image (HUFF/CDIC)
  bool withCover = false;
};

static Bytes buildMobi(const BookSpec& spec) {
  // ---- record 0: PalmDOC header (16) + MOBI header (232) + EXTH + title
  const uint32_t mobiHeaderLen = 0xE8;
  Bytes exth;
  {
    // records: 100 author, 503 title, 201 cover (index 0)
    Bytes recs;
    auto addRec = [&](uint32_t type, const Bytes& data) {
      Bytes r(8 + data.size());
      put32(r, 0, type);
      put32(r, 4, uint32_t(8 + data.size()));
      memcpy(&r[8], data.data(), data.size());
      append(recs, r);
    };
    addRec(100, Bytes(spec.author.begin(), spec.author.end()));
    addRec(524, Bytes({'e', 'n'}));
    if (spec.withCover) {
      Bytes v(4);
      put32(v, 0, 0);
      addRec(201, v);
    }
    exth.resize(12);
    memcpy(&exth[0], "EXTH", 4);
    put32(exth, 4, uint32_t(12 + recs.size()));  // padded below if needed
    put32(exth, 8, spec.withCover ? 3u : 2u);
    append(exth, recs);
    while (exth.size() % 4) exth.push_back(0);
    put32(exth, 4, uint32_t(exth.size()));
  }

  Bytes rec0(16 + mobiHeaderLen, 0);
  put16(rec0, 0, spec.compression);
  put32(rec0, 4, 0);  // text length, filled later
  put16(rec0, 8, uint32_t(spec.textRecords.size()));
  put16(rec0, 10, 4096);
  put16(rec0, 12, 0);  // encryption
  memcpy(&rec0[16], "MOBI", 4);
  put32(rec0, 20, mobiHeaderLen);
  put32(rec0, 24, 2);
  put32(rec0, 28, spec.encoding);
  put32(rec0, 0x68, 6);
  // record index of the first image, relative to all records (set below)
  put32(rec0, 0x80, 0x40);  // EXTH present
  put16(rec0, 0xF2, spec.extraFlags);
  rec0.resize(16 + mobiHeaderLen);
  append(rec0, exth);
  const uint32_t nameOff = uint32_t(rec0.size());
  append(rec0, spec.title);
  put32(rec0, 0x54, nameOff);
  put32(rec0, 0x58, uint32_t(spec.title.size()));

  // ---- PalmDB record list
  std::vector<Bytes> records;
  records.push_back(rec0);
  for (const Bytes& t : spec.textRecords) records.push_back(t);
  uint32_t firstImage = uint32_t(records.size());
  if (!spec.image.empty()) records.push_back(spec.image);
  for (const Bytes& r : spec.extraRecords) records.push_back(r);
  put32(records[0], 0x6C, firstImage);

  const size_t headerSize = 78 + 8 * records.size() + 2;
  Bytes file(headerSize, 0);
  memcpy(&file[0], "Synthetic", 9);
  memcpy(&file[60], "BOOK", 4);
  memcpy(&file[64], "MOBI", 4);
  put16(file, 76, uint32_t(records.size()));
  size_t off = headerSize;
  for (size_t i = 0; i < records.size(); ++i) {
    put32(file, 78 + 8 * i, uint32_t(off));
    file[78 + 8 * i + 4] = 0;  // attributes
    off += records[i].size();
  }
  for (const Bytes& r : records) append(file, r);
  return file;
}

// PalmDOC encoding helpers for hand-made test streams.
static Bytes literal(const std::string& s) { return Bytes(s.begin(), s.end()); }

// Builds a record with trailing entries. entries are payloads of trailing
// entries (their size incl. VLI is computed here); overlap adds the multibyte
// overlap descriptor.
static Bytes withTrailing(const Bytes& text, const Bytes& junk, bool overlap) {
  Bytes r = text;
  // layout read backwards by the parser: text | overlap | junk | VLI
  if (overlap) {
    // overlap descriptor low bits = 1 -> 2 bytes incl. the descriptor
    r.push_back(0x55);
    r.push_back(0x01);
  }
  if (!junk.empty()) {
    append(r, junk);
    r.push_back(uint8_t(0x80 | (junk.size() + 1)));  // VLI value includes itself
  }
  return r;
}

// ---------------------------------------------------------------------------
// Helpers to read the whole text stream
// ---------------------------------------------------------------------------

template <typename Doc>
static std::string readAll(Doc& doc, size_t chunkSize = 4096) {
  std::string out;
  std::vector<uint8_t> buf(chunkSize);
  for (;;) {
    size_t n = doc.read(buf.data(), buf.size());
    if (n == 0) break;
    out.append(reinterpret_cast<const char*>(buf.data()), n);
  }
  return out;
}

// Text of one section: reads until the stream moves on to the next section.
static std::string sectionText(MemoryMobi& doc, uint32_t section) {
  std::string out;
  if (doc.seek(Position(section, 0)) != Error::None) return out;
  for (;;) {
    uint8_t b = 0;
    if (doc.read(&b, 1) == 0) break;
    if (doc.position().section != section) break;  // this byte belongs to the next section
    out.push_back(static_cast<char>(b));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void testUtf8Markup() {
  BookSpec spec;
  spec.compression = 1;
  spec.textRecords.push_back(literal("<p>Hello</p>"));
  spec.textRecords.push_back(literal("<p>World &amp; more</p>"));
  Bytes file = buildMobi(spec);

  MemoryMobi doc(MemorySource(file.data(), file.size()));
  CHECK(doc.open() == Error::None);
  CHECK(doc.isOpen());
  CHECK(doc.info().compression == 1);
  CHECK(doc.info().utf8);
  CHECK(doc.seek(Position(2, 0)) == Error::OutOfRange);  // two sections
  CHECK(strcmp(doc.metadata().title, "Synthetic Title") == 0);
  CHECK(strcmp(doc.metadata().author, "Ada Lovelace") == 0);
  CHECK(strcmp(doc.metadata().language, "en") == 0);
  CHECK(readAll(doc) == "<p>Hello</p><p>World &amp; more</p>");
  CHECK(doc.read(nullptr, 0) == 0);
}

static void testPalmDocCompression() {
  // "abc" literal, back reference dist=3 len=6 -> "abcabcabc",
  // then 0xF8 -> " x" and literal newline.
  BookSpec spec;
  Bytes rec = {'a', 'b', 'c', 0x80, 0x1B, 0xF8, 0x0A};
  spec.textRecords.push_back(rec);
  // second record: literal run of 3 bytes (count prefix 0x03) then 'Z'
  spec.textRecords.push_back(Bytes({0x03, 'x', 'y', 'z', 'Z'}));
  Bytes file = buildMobi(spec);

  MemoryMobi doc(MemorySource(file.data(), file.size()));
  CHECK(doc.open() == Error::None);
  CHECK(doc.info().compression == 2);
  CHECK(readAll(doc) == "abcabcabc x\nxyzZ");

  // single byte reads must match
  MemoryMobi doc2(MemorySource(file.data(), file.size()));
  CHECK(doc2.open() == Error::None);
  CHECK(readAll(doc2, 1) == "abcabcabc x\nxyzZ");
}

static void testTrailingEntries() {
  // flags: bit0 (overlap) + bit1 (one VLI entry) -> extraFlags = 0x0003
  BookSpec spec;
  spec.compression = 1;
  spec.extraFlags = 0x0003;
  spec.textRecords.push_back(withTrailing(literal("first"), Bytes({0x11, 0x22}), true));
  spec.textRecords.push_back(withTrailing(literal("second"), Bytes({0x33}), true));
  Bytes file = buildMobi(spec);

  MemoryMobi doc(MemorySource(file.data(), file.size()));
  CHECK(doc.open() == Error::None);
  CHECK(readAll(doc) == "firstsecond");
}

static void testPlainMode() {
  // CP1252 book: é is 0xE9, € is &#8364;
  BookSpec spec;
  spec.compression = 1;
  spec.encoding = 1252;
  spec.textRecords.push_back(literal(
      "<p>Hello <b>world</b> &amp; caf\xE9 &#8364;</p>\n<p>Second&nbsp;para &lt;3</p>"));
  Bytes file = buildMobi(spec);

  MemoryMobi doc(MemorySource(file.data(), file.size()));
  CHECK(doc.open() == Error::None);
  CHECK(!doc.info().utf8);

  // markup mode converts CP1252 to UTF-8 but keeps the tags
  std::string markup = readAll(doc);
  CHECK(markup == "<p>Hello <b>world</b> &amp; caf\xC3\xA9 &#8364;</p>\n"
                  "<p>Second&nbsp;para &lt;3</p>");

  doc.setTextMode(TextMode::Plain);
  doc.rewind();
  std::string plain = readAll(doc);
  CHECK(plain == "Hello world & caf\xC3\xA9 \xE2\x82\xAC\nSecond\xC2\xA0" "para <3\n");
}

static void testPlainModeSmallReads() {
  BookSpec spec;
  spec.compression = 1;
  spec.textRecords.push_back(literal("<p>A&amp;B</p><p>&#x20AC;&hellip;</p>"));
  Bytes file = buildMobi(spec);

  MemoryMobi a(MemorySource(file.data(), file.size()), TextMode::Plain);
  CHECK(a.open() == Error::None);
  std::string whole = readAll(a, 4096);

  MemoryMobi b(MemorySource(file.data(), file.size()), TextMode::Plain);
  CHECK(b.open() == Error::None);
  std::string bytewise = readAll(b, 1);
  CHECK(whole == bytewise);
  CHECK(whole == "A&B\n\xE2\x82\xAC\xE2\x80\xA6\n");
}

static void testRandomAccessAndPosition() {
  BookSpec spec;
  spec.compression = 1;
  spec.textRecords.push_back(literal("chunk-zero"));
  spec.textRecords.push_back(literal("chunk-one"));
  spec.textRecords.push_back(literal("chunk-two"));
  Bytes file = buildMobi(spec);

  MemoryMobi doc(MemorySource(file.data(), file.size()));
  CHECK(doc.open() == Error::None);

  CHECK(doc.seek(Position(1, 0)) == Error::None);
  CHECK(readAll(doc) == "chunk-onechunk-two");
  CHECK(doc.seek(Position(3, 0)) == Error::OutOfRange);

  // read 3 bytes, remember position, read more, then restore
  CHECK(doc.rewind() == Error::None);
  uint8_t small[3];
  CHECK(doc.read(small, 3) == 3);
  Position p = doc.position();
  CHECK(p.section == 0 && p.offset == 3);
  CHECK(readAll(doc) == "nk-zerochunk-onechunk-two");
  CHECK(doc.seek(p) == Error::None);
  CHECK(readAll(doc) == "nk-zerochunk-onechunk-two");

  CHECK(doc.seek(Position(2, 6)) == Error::None);
  CHECK(readAll(doc) == "two");
  CHECK(doc.seek(Position(7, 0)) == Error::OutOfRange);
}

static void testSmallBuffer() {
  BookSpec spec;
  spec.compression = 1;
  spec.textRecords.push_back(literal(std::string(3000, 'q')));
  Bytes file = buildMobi(spec);
  MemoryMobi doc(MemorySource(file.data(), file.size()));
  CHECK(doc.open() == Error::None);

  // record buffer too small for the chunk -> error through read()
  MemoryMobi small(MemorySource(file.data(), file.size()));
  CHECK(small.open() == Error::None);
  BookSpec big;
  big.compression = 1;
  big.textRecords.push_back(literal(std::string(600, 'x')));
  Bytes file2 = buildMobi(big);
  MobiDocument<MemorySource, 512> tiny(MemorySource(file2.data(), file2.size()));
  CHECK(tiny.open() == Error::None);
  CHECK(readAll(tiny).empty());
  CHECK(tiny.lastError() == Error::BufferTooSmall);
}

static void testRawRecordsAndCover() {
  BookSpec spec;
  spec.compression = 1;
  spec.withCover = true;
  spec.textRecords.push_back(literal("text"));
  spec.image = Bytes({0xFF, 0xD8, 0xFF, 0xE0, 0x00});  // JPEG magic
  Bytes file = buildMobi(spec);

  MemoryMobi doc(MemorySource(file.data(), file.size()));
  CHECK(doc.open() == Error::None);
  CHECK(doc.info().firstImageRecord == 2);
  CHECK(doc.info().coverRecord == 2);

  uint8_t buf[16];
  size_t n = 0;
  CHECK(doc.readRecord(doc.info().coverRecord, buf, sizeof(buf), n) == Error::None);
  CHECK(n == 5 && buf[0] == 0xFF && buf[1] == 0xD8);
}

static void testErrors() {
  // too small
  uint8_t tiny[10] = {0};
  MemoryMobi a(MemorySource(tiny, sizeof(tiny)));
  CHECK(a.open() == Error::NotPalmDb);

  // not a MOBI (creator mismatch)
  Bytes junk(200, 0);
  MemoryMobi b(MemorySource(junk.data(), junk.size()));
  CHECK(b.open() == Error::NotMobi);

  // encrypted
  BookSpec enc;
  enc.compression = 1;
  enc.encryption = 2;
  enc.textRecords.push_back(literal("x"));
  Bytes file = buildMobi(enc);
  // set encryption field in record 0 (offset 12)
  uint32_t rec0 = (uint32_t(file[78]) << 24) | (uint32_t(file[79]) << 16) |
                  (uint32_t(file[80]) << 8) | uint32_t(file[81]);
  put16(file, rec0 + 12, 2);
  MemoryMobi c(MemorySource(file.data(), file.size()));
  CHECK(c.open() == Error::Encrypted);

  // HUFF/CDIC is reported as unsupported
  BookSpec huff;
  huff.compression = 1;
  huff.textRecords.push_back(literal("x"));
  Bytes hf = buildMobi(huff);
  uint32_t r0 = (uint32_t(hf[78]) << 24) | (uint32_t(hf[79]) << 16) |
                (uint32_t(hf[80]) << 8) | uint32_t(hf[81]);
  put16(hf, r0, 17480);
  MemoryMobi d(MemorySource(hf.data(), hf.size()));
  CHECK(d.open() == Error::CorruptData);  // no HUFF tables present

  // corrupt back reference (distance beyond output start)
  BookSpec bad;
  bad.textRecords.push_back(Bytes({0x90, 0x00}));  // dist 18 at start
  Bytes bf = buildMobi(bad);
  MemoryMobi e(MemorySource(bf.data(), bf.size()));
  CHECK(e.open() == Error::None);
  CHECK(e.seek(Position(0, 0)) == Error::CorruptData);
}

static void testEmptyChunksAndOverlap() {
  BookSpec spec;
  spec.compression = 1;
  spec.textRecords.push_back(literal("one"));
  spec.textRecords.push_back(Bytes());
  spec.textRecords.push_back(literal("two"));
  Bytes file = buildMobi(spec);
  MemoryMobi doc(MemorySource(file.data(), file.size()));
  CHECK(doc.open() == Error::None);
  CHECK(readAll(doc) == "onetwo");
}


// ---------------------------------------------------------------------------
// WithKf8 = false: the text works, KF8 calls report Error::Disabled
// ---------------------------------------------------------------------------

static void testNoKf8Build() {
  BookSpec spec;
  spec.compression = 1;
  spec.textRecords.push_back(literal("<p>plain</p>"));
  Bytes file = buildMobi(spec);

  MemoryMobiNoKf8 doc(MemorySource(file.data(), file.size()));
  CHECK(doc.open() == Error::None);
  CHECK(readAll(doc) == "<p>plain</p>");
  CHECK(sizeof(MemoryMobiNoKf8) < sizeof(MemoryMobi));
}

// ---------------------------------------------------------------------------
// Plain text skips the metadata in <head> and <title>
// ---------------------------------------------------------------------------

static void testPlainSkipsHead() {
  BookSpec spec;
  spec.compression = 1;
  spec.textRecords.push_back(literal(
      "<html><head><title>Velocity</title><meta name=\"x\"/></head>"
      "<body><h1>What is DevOps?</h1><p>body text</p></body></html>"));
  Bytes file = buildMobi(spec);
  MemoryMobi doc(MemorySource(file.data(), file.size()), TextMode::Plain);
  CHECK(doc.open() == Error::None);
  CHECK(readAll(doc) == "What is DevOps?\nbody text\n");
  // markup mode keeps everything
  doc.setTextMode(TextMode::Markup);
  doc.rewind();
  CHECK(readAll(doc).find("<title>Velocity</title>") != std::string::npos);
}

static void testPlainSkipsUnclosedHead() {
  // the head is never closed: the <body> tag still ends the skip
  BookSpec spec;
  spec.compression = 1;
  spec.textRecords.push_back(literal("<head><title>Velocity<body><p>hi</p>"));
  Bytes file = buildMobi(spec);
  MemoryMobi doc(MemorySource(file.data(), file.size()), TextMode::Plain);
  CHECK(doc.open() == Error::None);
  CHECK(readAll(doc) == "hi\n");
}

// ---------------------------------------------------------------------------
// Trailing entries longer than 64 bytes (regression: the tail used to be
// limited to 64 bytes)
// ---------------------------------------------------------------------------

static void testLongTrailingEntry() {
  BookSpec spec;
  spec.compression = 1;
  spec.extraFlags = 0x0002;  // one trailing entry
  Bytes junk(100, 0x42);     // entry = 100 junk bytes + 1 VLI byte
  spec.textRecords.push_back(withTrailing(literal("long-tail"), junk, false));
  Bytes file = buildMobi(spec);
  MemoryMobi doc(MemorySource(file.data(), file.size()));
  CHECK(doc.open() == Error::None);
  CHECK(readAll(doc) == "long-tail");
}

// ---------------------------------------------------------------------------
// Image references (recindex and kindle:embed, both 1 based)
// ---------------------------------------------------------------------------

static void testImageRefs() {
  BookSpec spec;
  spec.compression = 1;
  spec.textRecords.push_back(literal(
      "<p>a</p><img src=\"x.jpg\" recindex=\"1\"/>b<IMG SRC='kindle:embed:0002?mime=image/png'>c"
      "<img recindex='9'>"));
  spec.image = Bytes({0xFF, 0xD8, 0xFF, 0xE0});                         // image 1
  spec.extraRecords.push_back(Bytes({0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'}));  // image 2
  Bytes file = buildMobi(spec);

  MemoryMobi doc(MemorySource(file.data(), file.size()));
  CHECK(doc.open() == Error::None);

  std::vector<ImageRef> refs;
  Error e = doc.forEachImageRef([&](const ImageRef& r) -> bool {
    refs.push_back(r);
    return true;
  });
  CHECK(e == Error::None);
  CHECK(refs.size() == 3);
  if (refs.size() == 3) {
    CHECK(refs[0].position == Position(0, 8) && refs[0].record == 2);
    CHECK(refs[1].record == 3);       // kindle:embed:0002 -> image 2
    CHECK(refs[2].record == kNoRecord);  // image 9 does not exist
  }

  // the callback can stop the scan early
  size_t seen = 0;
  doc.forEachImageRef([&](const ImageRef&) -> bool {
    ++seen;
    return false;
  });
  CHECK(seen == 1);
}

// ---------------------------------------------------------------------------
// HUFF/CDIC: a small encoder that produces 8 bit codes. Code byte for phrase
// r is (255 - r), the decoder maps it back to r.
// ---------------------------------------------------------------------------

static void patch32(Bytes& b, size_t pos, uint32_t v) { put32(b, pos, v); }
static void patch16(Bytes& b, size_t pos, uint32_t v) { put16(b, pos, v); }

static uint32_t recordOffsetOf(const Bytes& file, uint32_t i) {
  return (uint32_t(file[78 + 8 * i]) << 24) | (uint32_t(file[79 + 8 * i]) << 16) |
         (uint32_t(file[80 + 8 * i]) << 8) | uint32_t(file[81 + 8 * i]);
}

// HUFF record with the terminal (8 bit) table only
static Bytes buildHuffRecord(uint32_t phrases) {
  Bytes h(24 + 1024 + 256, 0);
  memcpy(&h[0], "HUFF\0\0\0\x18", 8);
  put32(h, 8, 24);
  put32(h, 12, 24 + 1024);
  for (size_t i = 0; i < 256; ++i) put32(h, 24 + 4 * i, (255u << 8) | 0x80 | 8);
  // length 8: min = 255 - phrases + 1, max = 255
  put32(h, 24 + 1024 + 4 * 14, 255 - phrases + 1);
  put32(h, 24 + 1024 + 4 * 15, 255);
  return h;
}

// CDIC record holding n phrases; each phrase is (flagged length, data)
struct CdicPhrase {
  bool stored;  // true: plain text, false: huffman coded
  Bytes data;
};

static Bytes buildCdicRecord(uint32_t phrases, uint32_t bits, const std::vector<CdicPhrase>& ps) {
  Bytes r(16 + 2 * ps.size(), 0);
  memcpy(&r[0], "CDIC\0\0\0\x10", 8);
  put32(r, 8, phrases);
  put32(r, 12, bits);
  uint32_t off = uint32_t(2 * ps.size());
  for (size_t i = 0; i < ps.size(); ++i) {
    put16(r, 16 + 2 * i, off);
    uint32_t blen = uint32_t(ps[i].data.size()) | (ps[i].stored ? 0x8000u : 0);
    Bytes entry(2);
    put16(entry, 0, blen);
    append(entry, ps[i].data);
    append(r, entry);
    off += uint32_t(entry.size());
  }
  return r;
}

static void testHuffCdic() {
  // phrases: 0 "hello ", 1 "world", 2 = [0 1], 3 "!", 4 = [2 3], 5 "\n"
  const uint32_t kPhrases = 6;
  const uint32_t kBits = 2;  // 4 phrases per CDIC record
  std::vector<CdicPhrase> c0 = {
      {true, literal("hello ")},
      {true, literal("world")},
      {false, Bytes({uint8_t(255 - 0), uint8_t(255 - 1)})},
      {true, literal("!")},
  };
  std::vector<CdicPhrase> c1 = {
      {false, Bytes({uint8_t(255 - 2), uint8_t(255 - 3)})},
      {true, literal("\n")},
  };
  // text record: [4] [0] [5] [2]
  Bytes text = {uint8_t(255 - 4), uint8_t(255 - 0), uint8_t(255 - 5), uint8_t(255 - 2)};

  BookSpec spec;
  spec.compression = 1;  // placeholder, patched to 17480 below
  spec.textRecords.push_back(text);
  spec.extraRecords.push_back(buildHuffRecord(kPhrases));
  spec.extraRecords.push_back(buildCdicRecord(kPhrases, kBits, c0));
  spec.extraRecords.push_back(buildCdicRecord(kPhrases, kBits, c1));
  Bytes file = buildMobi(spec);

  // record 0 fields: compression 17480, huffman record index and count
  uint32_t rec0 = recordOffsetOf(file, 0);
  patch16(file, rec0 + 0, 17480);
  patch32(file, rec0 + 0x70, 2);  // text record is 1, HUFF record is 2
  patch32(file, rec0 + 0x74, 3);  // HUFF + two CDIC records

  MemoryMobi doc(MemorySource(file.data(), file.size()));
  CHECK(doc.open() == Error::None);
  CHECK(doc.info().compression == 17480);
  CHECK(readAll(doc) == "hello world!hello \nhello world");

  // single byte reads and a seek into the middle of the chunk
  MemoryMobi doc2(MemorySource(file.data(), file.size()));
  CHECK(doc2.open() == Error::None);
  CHECK(readAll(doc2, 1) == "hello world!hello \nhello world");

  // a code that points beyond the dictionary is reported as corrupt
  Bytes bad = {uint8_t(255 - 9)};
  BookSpec badSpec;
  badSpec.compression = 1;
  badSpec.textRecords.push_back(bad);
  badSpec.extraRecords = spec.extraRecords;
  Bytes badFile = buildMobi(badSpec);
  uint32_t badRec0 = recordOffsetOf(badFile, 0);
  patch16(badFile, badRec0 + 0, 17480);
  patch32(badFile, badRec0 + 0x70, 2);
  patch32(badFile, badRec0 + 0x74, 3);
  MemoryMobi bd(MemorySource(badFile.data(), badFile.size()));
  CHECK(bd.open() == Error::None);
  CHECK(bd.seek(Position(0, 0)) == Error::CorruptData);
}

static void testImageType() {
  const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0};
  const uint8_t png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  const uint8_t gif[] = {'G', 'I', 'F', '8', '9', 'a'};
  const uint8_t bmp[] = {'B', 'M', 0, 0};
  const uint8_t other[] = {0x00, 0x01, 0x02, 0x03};
  CHECK(imageType(jpeg, sizeof(jpeg)) == ImageType::Jpeg);
  CHECK(imageType(png, sizeof(png)) == ImageType::Png);
  CHECK(imageType(gif, sizeof(gif)) == ImageType::Gif);
  CHECK(imageType(bmp, sizeof(bmp)) == ImageType::Bmp);
  CHECK(imageType(other, sizeof(other)) == ImageType::Unknown);
  CHECK(imageType(png, 4) == ImageType::Unknown);  // too short for the signature
  CHECK(imageType(nullptr, 0) == ImageType::Unknown);
}

// ---------------------------------------------------------------------------
// Smoke test on real files (optional, from the command line)
// ---------------------------------------------------------------------------

static void smokeFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    printf("cannot open %s\n", path);
    ++g_failures;
    return;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  Bytes data((size_t(len)));
  if (fread(data.data(), 1, data.size(), f) != data.size()) {
    fclose(f);
    ++g_failures;
    return;
  }
  fclose(f);

  MemoryMobi doc(MemorySource(data.data(), data.size()));
  Error e = doc.open();
  if (e != Error::None) {
    printf("%-60s open: %s\n", path, errorName(e));
    ++g_failures;
    return;
  }
  // stream the whole book in both modes, checking that every chunk decodes
  size_t totalMarkup = 0, totalPlain = 0;
  std::vector<uint8_t> buf(777);
  doc.setTextMode(TextMode::Markup);
  doc.rewind();
  for (;;) {
    size_t n = doc.read(buf.data(), buf.size());
    if (n == 0) break;
    totalMarkup += n;
  }
  CHECK(doc.lastError() == Error::None);
  // the markup stream must contain exactly the announced text length
  // MOBI6 streams have exactly the announced length; KF8 streams (AZW3 and
  // hybrid books) deliver the assembled KF8 text, which is checked by sections below
  bool kf8Stream = doc.info().isKf8 || doc.info().kf8Boundary != kNoRecord;
  if (!kf8Stream) CHECK(totalMarkup == doc.info().textLength);
  doc.setTextMode(TextMode::Plain);
  doc.rewind();
  std::string plainHead;
  for (;;) {
    size_t n = doc.read(buf.data(), buf.size());
    if (n == 0) break;
    if (plainHead.size() < 200) plainHead.append(reinterpret_cast<char*>(buf.data()), n);
    totalPlain += n;
  }
  CHECK(doc.lastError() == Error::None);
  // table of contents: every entry lies inside the text and maps to a stream position
  {
    uint32_t tocN = 0;
    CHECK(doc.tocCount(tocN) == Error::None);
    uint32_t visited = 0;
    size_t shown = 0;
    Error e = doc.forEachTocEntry([&](const TocEntry& te) -> bool {
      // a resolved start must be a position the stream can seek to
      if (te.start.section != kNoRecord) CHECK(doc.seek(te.start) == Error::None);
      if (shown < 3) {
        printf("      toc[%u] level=%u start=%u:%u \"%s\"\n", te.index, te.level,
               te.start.section, te.start.offset, te.title);
        ++shown;
      }
      ++visited;
      return true;
    });
    CHECK(e == Error::None);
    CHECK(visited == tocN);
    printf("    toc entries=%u\n", tocN);
  }

  // cover: the record must carry a known image signature
  if (doc.info().coverRecord != kNoRecord) {
    std::vector<uint8_t> img(1 << 20);
    size_t n = 0;
    CHECK(doc.readRecord(doc.info().coverRecord, img.data(), img.size(), n) == Error::None);
    CHECK(imageType(img.data(), n) != ImageType::Unknown);
    uint32_t len = 0;
    CHECK(doc.recordLength(doc.info().coverRecord, len) == Error::None && len == n);
    printf("    cover type=%d bytes=%zu\n", int(imageType(img.data(), n)), n);
  }

  // image references: every tag must resolve to a recognized image
  {
    std::vector<uint8_t> chunk(8192), img(1 << 20);
    size_t refs = 0, bad = 0;
    Error e2 = doc.forEachImageRef([&](const ImageRef& r) -> bool {
      ++refs;
      size_t n = 0;
      if (r.record == kNoRecord || doc.readRecord(r.record, img.data(), img.size(), n) != Error::None ||
          imageType(img.data(), n) == ImageType::Unknown) {
        ++bad;
      }
      return true;
    });
    CHECK(e2 == Error::None);
    CHECK(bad == 0);
    printf("    image refs=%zu\n", refs);
  }

  // sections: they follow each other, and their text is the stream text
  {
    doc.setTextMode(TextMode::Markup);
    CHECK(doc.rewind() == Error::None);
    std::string all = readAll(doc);
    std::string joined;
    uint32_t sections = 0;
    while (doc.seek(Position(sections, 0)) == Error::None) {
      joined += sectionText(doc, sections);
      ++sections;
    }
    CHECK(sections > 0);
    CHECK(joined == all);
    CHECK(doc.seek(Position(0, 0)) == Error::None);
    printf("    sections=%u\n", sections);
  }

  const Metadata& m = doc.metadata();
  printf("%-60s v%u %s textLen=%u markup=%zu plain=%zu\n", path, doc.info().mobiVersion,
         doc.info().utf8 ? "utf8" : "cp1252", doc.info().textLength, totalMarkup, totalPlain);
  printf("    title=\"%s\" author=\"%s\" cover=%s\n", m.title, m.author,
         doc.info().coverRecord == kNoRecord ? "none" : "yes");
  printf("    plain head: \"%.80s\"\n", plainHead.c_str());
}

// ---------------------------------------------------------------------------
// Links (TextMode::PlainLinks): markers around the link text and resolveLink()
// ---------------------------------------------------------------------------
static void testPlainLinks() {
  BookSpec spec;
  spec.compression = 1;
  // "Preface" starts at offset 0 of the text record, the chapter at offset 30
  const std::string body = "<p>Intro <a  filepos=0000000030 >Chapter</a> and <a href=\"mailto:x@y.z\">mail</a>.</p>";
  spec.textRecords.push_back(literal("<html><body>" + body + "<h2>Chapter one</h2></body></html>"));
  Bytes file = buildMobi(spec);
  MemoryMobiNoKf8 doc(MemorySource(file.data(), file.size()), TextMode::PlainLinks);
  CHECK(doc.open() == Error::None);
  std::string out = readAll(doc);
  // the filepos link is marked, the mailto link is not
  CHECK(out.find("\x01" "filepos=0000000030" "\x02" "Chapter\x03") != std::string::npos);
  CHECK(out.find("mail") != std::string::npos);
  CHECK(out.find("mailto") == std::string::npos);
  // resolveLink: the target is the offset of "Chapter one" in the text
  Position p;
  CHECK(doc.resolveLink("filepos=0000000030", p));
  CHECK(p.section == 0 && p.offset == 30);
  CHECK(!doc.resolveLink("filepos=99999999", p));
  CHECK(!doc.resolveLink("http://example.com", p));
  // headings are wrapped in markers on their own lines
  CHECK(out.find("\n\x04" "Chapter one\x05\n") != std::string::npos);
  // markup mode is unchanged
  doc.setTextMode(TextMode::Markup);
  doc.rewind();
  CHECK(readAll(doc).find("filepos=0000000030") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Images in plain text with markers (TextMode::PlainLinks)
// ---------------------------------------------------------------------------
static void testPlainImages() {
  BookSpec spec;
  spec.compression = 1;
  spec.image = literal("\x89PNG-fake");
  spec.textRecords.push_back(literal("<html><body><p>Before</p><img recindex=\"1\"/><p>After</p></body></html>"));
  Bytes file = buildMobi(spec);
  MemoryMobiNoKf8 doc(MemorySource(file.data(), file.size()), TextMode::PlainLinks);
  CHECK(doc.open() == Error::None);
  uint32_t record = kNoRecord;
  doc.forEachImageRef([&](const ImageRef& ref) -> bool {
    record = ref.record;
    return false;
  });
  CHECK(record != kNoRecord);
  const std::string out = readAll(doc);
  // the image is a paragraph of its own, marked with its record number
  const std::string marker = std::string("\x06") + std::to_string(record) + "\x07";
  CHECK(out.find("\n\n" + marker + "\n") != std::string::npos);
  CHECK(out.find("Before") < out.find(marker) && out.find(marker) < out.find("After"));
}

int main(int argc, char** argv) {
  testUtf8Markup();
  testPalmDocCompression();
  testTrailingEntries();
  testPlainMode();
  testPlainModeSmallReads();
  testRandomAccessAndPosition();
  testSmallBuffer();
  testRawRecordsAndCover();
  testErrors();
  testEmptyChunksAndOverlap();
  testHuffCdic();
  testImageType();
  testImageRefs();
  testNoKf8Build();
  testLongTrailingEntry();
  testPlainSkipsHead();
  testPlainSkipsUnclosedHead();
  testPlainLinks();
  testPlainImages();

  for (int i = 1; i < argc; ++i) smokeFile(argv[i]);

  if (g_failures == 0) {
    printf("ALL TESTS PASSED\n");
    return 0;
  }
  printf("%d FAILURE(S)\n", g_failures);
  return 1;
}
