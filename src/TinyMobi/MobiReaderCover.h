#pragma once
/**
 * @file MobiReaderCover.h
 * @brief The JPEG cover of a book for the reader GUI: decoded with TinyJPEG
 * into a sprite, and shown full screen as a page of its own.
 */
#include <functional>
#include <memory>
#include <vector>

#include "TinyJPEGDecoder.h"
#include "TinyMaterialDesign.h"
#include "TinyMobi.h"

namespace tinymobi {

/**
 * @brief The cover of a book. load() decodes the JPEG cover of the open
 * document into a sprite scaled to the largest size that fits the screen; view is the widget that
 * shows it. Covers in other formats are not shown.
 */
template <typename RGB_T = TINYMD_DEFAULT_RGB_T, typename Doc = MemoryMobi>
class MobiCover {
 public:
  /// \brief The cover page: the image centred on the full screen. A tap or a swipe
    /// to the left shows the text.
    class Page : public tinymd::Widget<RGB_T> {
     public:
      /// \brief Called when the page is tapped or swiped to the left.
      std::function<void()> onNext;

      /// \brief Sets the image to show (the image is not copied).
      void setImage(const tinygpu::ISurface<RGB_T>* image) { image_ = image; }

      /// \brief Draws the black background and the image, centred.
      void draw(tinygpu::ISurface<RGB_T>& target) override {
        target.fillRect(static_cast<size_t>(this->bounds.x), static_cast<size_t>(this->bounds.y),
                        static_cast<size_t>(this->bounds.w), static_cast<size_t>(this->bounds.h), RGB_T(0, 0, 0));
        if (image_ == nullptr) return;
        const int32_t x = this->bounds.x + (this->bounds.w - static_cast<int32_t>(image_->width())) / 2;
        const int32_t y = this->bounds.y + (this->bounds.h - static_cast<int32_t>(image_->height())) / 2;
        target.drawSprite(static_cast<size_t>(x < 0 ? 0 : x), static_cast<size_t>(y < 0 ? 0 : y), *image_);
      }

      /// \brief A tap or a swipe to the left calls onNext.
      bool onGesture(const tinygpu::GestureEvent& event) override {
        if (event.phase != tinygpu::GesturePhase::kEnded) return false;
        if (event.type == tinygpu::GestureType::kTap || event.type == tinygpu::GestureType::kSwipeLeft) {
          if (onNext) onNext();
          return true;
        }
        return false;
      }

     private:
      const tinygpu::ISurface<RGB_T>* image_ = nullptr;
    };


  /// \brief The page widget that shows the cover (full screen).
  Page view;

  /// \brief True if the book has a JPEG cover that was decoded.
  bool has() const { return has_; }

  /// \brief Removes the cover.
  void reset() {
    has_ = false;
    view.setImage(nullptr);
    cover_.reset();
  }

  /// \brief Decodes the cover (a JPEG) into a sprite scaled to fit the top of the
    /// first page. Without a JPEG cover, nothing is shown.
    /// \brief Loads the cover of the open document, scaled to fit a width x height screen.
    /// Returns true if there is a JPEG cover.
    bool load(Doc& doc, int32_t width, int32_t height) {
      reset();
      const uint32_t cover = doc.info().coverRecord;
      uint32_t len = 0;
      if (cover == kNoRecord || doc.recordLength(cover, len) != Error::None || len == 0) return false;
      std::vector<uint8_t> bytes(len);
      size_t n = 0;
      if (doc.readRecord(cover, bytes.data(), bytes.size(), n) != Error::None) return false;
      if (imageType(bytes.data(), n) != ImageType::Jpeg) return false;

      tinyjpeg::TinyJPEGDecoder decoder;
      uint16_t jw = 0, jh = 0;
      if (decoder.getJpgSize(&jw, &jh, bytes.data(), n) != JDR_OK || jw == 0 || jh == 0) return false;
      // the largest size with the aspect ratio of the image that fits the screen
      // (nothing is cut off); the image is centred, the rest of the screen is black
      const uint64_t W = static_cast<uint64_t>(width), H = static_cast<uint64_t>(height);
      uint64_t tw = W, th = H;
      if (static_cast<uint64_t>(jw) * H <= static_cast<uint64_t>(jh) * W) {
        tw = static_cast<uint64_t>(jw) * H / jh;
      } else {
        th = static_cast<uint64_t>(jh) * W / jw;
      }
      if (tw == 0 || th == 0) return false;
      // decode at the largest scale (1, 1/2, 1/4, 1/8) that is still at least the target size
      uint8_t scale = 1;
      for (uint8_t s = 8; s >= 1; s = static_cast<uint8_t>(s / 2)) {
        const uint64_t sw = (jw + s - 1) / s, sh = (jh + s - 1) / s;
        if (sw >= tw && sh >= th) {
          scale = s;
          break;
        }
      }
      decodedW_ = (jw + scale - 1) / scale;
      decodedH_ = (jh + scale - 1) / scale;
      targetW_ = static_cast<uint32_t>(tw);
      targetH_ = static_cast<uint32_t>(th);
      offsetX_ = static_cast<int32_t>((W - tw) / 2);
      offsetY_ = static_cast<int32_t>((H - th) / 2);
      cover_.reset(new tinygpu::Surface<RGB_T>(static_cast<size_t>(width), static_cast<size_t>(height), tinygpu::FontRGB565));
      if (!cover_->begin()) {  // allocates the pixel buffer
        cover_.reset();
        return false;
      }
      cover_->clear();
      decoder.setJpgScale(scale);
      decoder.setUserData(this);
      decoder.setCallback(&MobiCover::onCoverBlock);
      if (decoder.drawJpg(0, 0, bytes.data(), n) != JDR_OK) {
        cover_.reset();
        return false;
      }
      view.setImage(cover_.get());
      view.bounds = tinymd::Bounds(0, 0, width, height);
      has_ = true;
      return true;
    }

    /// \brief Receives the decoded blocks of the cover (TinyJPEG callback). Each decoded
    /// pixel is written to the target pixels it covers (nearest neighbour).
    static bool onCoverBlock(tinyjpeg::TinyJPEGDecoder& decoder, int16_t x, int16_t y, uint16_t w,
                             uint16_t h, uint16_t* data) {
      MobiCover* self = static_cast<MobiCover*>(decoder.getUserData());
      if (self == nullptr || !self->cover_) return false;
      tinygpu::ISurface<RGB_T>& target = *self->cover_;
      const uint64_t dw = self->decodedW_, dh = self->decodedH_;
      const uint64_t tw = self->targetW_, th = self->targetH_;
      for (uint16_t j = 0; j < h; ++j) {
        const uint64_t dy = static_cast<uint64_t>(y) + j;
        if (dy >= dh) break;
        const uint64_t ty0 = dy * th / dh, ty1 = (dy + 1) * th / dh;
        for (uint16_t i = 0; i < w; ++i) {
          const uint64_t dx = static_cast<uint64_t>(x) + i;
          if (dx >= dw) break;
          const uint64_t tx0 = dx * tw / dw, tx1 = (dx + 1) * tw / dw;
          const RGB_T color = fromRGB565(data[j * w + i]);
          for (uint64_t ty = ty0; ty < ty1; ++ty) {
            for (uint64_t tx = tx0; tx < tx1; ++tx) {
              const int64_t px = self->offsetX_ + static_cast<int64_t>(tx);
              const int64_t py = self->offsetY_ + static_cast<int64_t>(ty);
              if (px >= 0 && py >= 0 && px < static_cast<int64_t>(target.width()) &&
                  py < static_cast<int64_t>(target.height())) {
                target.setPixel(static_cast<size_t>(px), static_cast<size_t>(py), color);
              }
            }
          }
        }
      }
      return true;
    }

    /// \brief Converts a 16 bit RGB565 pixel to RGB_T.
    static RGB_T fromRGB565(uint16_t raw) {
      return RGB_T(static_cast<uint8_t>(((raw >> 11) & 0x1F) << 3),
                   static_cast<uint8_t>(((raw >> 5) & 0x3F) << 2),
                   static_cast<uint8_t>((raw & 0x1F) << 3));
    }


 private:
  std::unique_ptr<tinygpu::Surface<RGB_T>> cover_;  ///< the decoded cover, null if none
  bool has_ = false;
  uint32_t decodedW_ = 0, decodedH_ = 0;  ///< size of the decoded image (at its scale)
  uint32_t targetW_ = 0, targetH_ = 0;    ///< size of the image on the screen
  int32_t offsetX_ = 0, offsetY_ = 0;    ///< where the image starts on the screen
};

}  // namespace tinymobi
