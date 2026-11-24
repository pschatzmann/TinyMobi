// TinyMobi - image decoding for the reader GUI: JPEG (TinyJPEG), PNG and GIF
// (the TinyGPU parsers), scaled to fit a box.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

#include "TinyGPU.h"  // the umbrella header: the TinyGPU headers depend on each other
#include "TinyGPU/IO/GIFParser.h"
#include "TinyGPU/IO/PNGParser.h"
#include "TinyJPEGDecoder.h"
#include "TinyMobi.h"

namespace tinymobi {

namespace detail {

// A TinyJPEG block is written into the surface (RGB565 pixels from the decoder).
template <typename RGB_T>
bool writeJpegBlock(tinyjpeg::TinyJPEGDecoder& decoder, int16_t x, int16_t y, uint16_t w, uint16_t h,
                    uint16_t* data) {
  tinygpu::Surface<RGB_T>* dst = static_cast<tinygpu::Surface<RGB_T>*>(decoder.getUserData());
  if (dst == nullptr) return false;
  for (uint16_t j = 0; j < h; ++j) {
    for (uint16_t i = 0; i < w; ++i) {
      const int32_t px = static_cast<int32_t>(x) + i;
      const int32_t py = static_cast<int32_t>(y) + j;
      if (px < 0 || py < 0 || px >= static_cast<int32_t>(dst->width()) ||
          py >= static_cast<int32_t>(dst->height())) {
        continue;
      }
      const uint16_t raw = data[j * w + i];
      dst->setPixel(static_cast<size_t>(px), static_cast<size_t>(py),
                    RGB_T(static_cast<uint8_t>(((raw >> 11) & 0x1F) << 3),
                          static_cast<uint8_t>(((raw >> 5) & 0x3F) << 2),
                          static_cast<uint8_t>((raw & 0x1F) << 3)));
    }
  }
  return true;
}

template <typename RGB_T>
bool decodeJpegFull(const uint8_t* data, size_t n, tinygpu::Surface<RGB_T>& dst) {
  tinyjpeg::TinyJPEGDecoder decoder;
  uint16_t jw = 0, jh = 0;
  if (decoder.getJpgSize(&jw, &jh, data, n) != JDR_OK || jw == 0 || jh == 0) return false;
  if (!dst.resize(jw, jh) || !dst.begin()) return false;
  decoder.setJpgScale(1);
  decoder.setUserData(&dst);
  decoder.setCallback(&writeJpegBlock<RGB_T>);
  return decoder.drawJpg(0, 0, data, n) == JDR_OK;
}

}  // namespace detail

/// Decodes a JPEG, PNG or GIF image and scales it to fit maxW x maxH, keeping the
/// proportions. Images are not enlarged. Returns null if the data is not an image
/// this reads, or if it is corrupt.
template <typename RGB_T>
std::shared_ptr<tinygpu::Surface<RGB_T>> decodeImageFit(const uint8_t* data, size_t n, size_t maxW, size_t maxH) {
  if (data == nullptr || n == 0 || maxW == 0 || maxH == 0) return nullptr;
  std::unique_ptr<tinygpu::Surface<RGB_T>> src(new tinygpu::Surface<RGB_T>(1, 1, tinygpu::FontRGB565));
  switch (imageType(data, n)) {
    case ImageType::Jpeg:
      if (!detail::decodeJpegFull(data, n, *src)) return nullptr;
      break;
    case ImageType::Png: {
      tinygpu::PNGParser<RGB_T> parser(*src);
      if (!parser.decode(data, n)) return nullptr;
      break;
    }
    case ImageType::Gif: {
      tinygpu::GIFParser<RGB_T> parser(*src);
      if (!parser.decode(data, n)) return nullptr;
      break;
    }
    default:
      return nullptr;
  }
  const size_t sw = src->width(), sh = src->height();
  if (sw == 0 || sh == 0) return nullptr;
  size_t tw = sw, th = sh;
  if (sw > maxW || sh > maxH) {
    if (sw * maxH <= sh * maxW) {
      tw = sw * maxH / sh;
      th = maxH;
    } else {
      th = sh * maxW / sw;
      tw = maxW;
    }
  }
  if (tw == 0) tw = 1;
  if (th == 0) th = 1;
  std::shared_ptr<tinygpu::Surface<RGB_T>> out =
      std::make_shared<tinygpu::Surface<RGB_T>>(tw, th, tinygpu::FontRGB565);
  if (!out->begin()) return nullptr;
  // nearest neighbour
  for (size_t y = 0; y < th; ++y) {
    for (size_t x = 0; x < tw; ++x) {
      out->setPixel(x, y, src->getPixel(x * sw / tw, y * sh / th));
    }
  }
  return out;
}

}  // namespace tinymobi
