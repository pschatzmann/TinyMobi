# Running the example on the desktop

[Back to the README](../README.md)

`examples/serial-reader` also builds as a desktop program with the
[Arduino emulator](https://github.com/pschatzmann/arduino-emulator). The
emulator is fetched once by CMake. To use a local checkout, pass
`-DFETCHCONTENT_SOURCE_DIR_ARDUINO_EMULATOR=<path>`.

```sh
cmake -S examples/serial-reader -B build-serial-reader -DTINYMOBI_EXAMPLE_BOOK=/path/to/book.mobi
cmake --build build-serial-reader
./build-serial-reader/serial-reader
```

The emulator's SD card uses host paths. `TINYMOBI_EXAMPLE_BOOK` is therefore
the path of the book on your machine, and the cover is written to
`cover.<ext>` in the working directory (change the name with
`TINYMOBI_EXAMPLE_COVER`).
