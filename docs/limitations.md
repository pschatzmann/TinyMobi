# Limitations

[Back to the README](../README.md)

- DRM: reported as `Error::Encrypted`.
- The FDST flow table is not parsed. The KF8 text is assumed to start at
  offset 0, which holds for the books tested.
- The repair path for malformed KF8 fragment positions (KindleUnpack's
  fallback) is not ported. Badly formed KF8 books may assemble differently.
- Links are resolved from the link text to the position of its target. The
  KF8 links (`kindle:pos`) are mapped through the fragment table, which was
  checked against the text of the books tested. Links that name a place by its
  text (cross-references) can land a few lines away from the words of the link.
- Reading a page of a KF8 book starts from the beginning of its part, so turning
  pages takes longer in long parts. MOBI6 books are not affected.
- `TocEntry::start` (the NCX index) is only resolved for MOBI6 books; for KF8
  books its `section` is `kNoRecord`. Use the links in the text instead.
- Images are not decoded. The bytes go to a decoder library of your choice.
- Hybrid books are read through their KF8 text, not their MOBI6 text.
