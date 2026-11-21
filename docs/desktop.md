# Desktop build and tests

[Back to the README](../README.md)

Requires CMake 3.16+ and a C++11 compiler.

```sh
cmake -S . -B build -DTINYMOBI_SANITIZE=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

- `tinymobi.synthetic` builds small MOBI files in memory and checks the output
  byte for byte. It covers compression, trailing entries, CP1252, plain mode,
  seeking, metadata, the cover, image references, HUFF/CDIC and error
  handling. It always runs.
- `tinymobi.samples` parses and streams real books. Enable it with
  `-DTINYMOBI_SAMPLE_DIR=/path/to/books`. Every `.mobi`, `.azw` and `.azw3`
  below that directory must open and stream exactly `textLength` bytes. The
  table of contents, image references, the sections of KF8 books and the cover are checked too.

`TINYMOBI_SANITIZE=ON` builds the tests with AddressSanitizer and UBSan.

## Real HUFF/CDIC books

The HUFF/CDIC test files are not in this repository. Point
`TINYMOBI_SAMPLE_DIR` at a folder with them:

- `sample-unicode-huffdic.mobi` from the
  [libmobi](https://github.com/bfabiszewski/libmobi) test samples
- `en_huffdic.mobi`, `en_huffdic_stale_index.mobi` and `ja_huffdic.mobi` from
  the [kindling](https://github.com/ciscoriordan/kindling) test fixtures
