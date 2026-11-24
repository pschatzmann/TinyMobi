#pragma once
/**
 * @file MobiReaderApp.h
 * @brief A page-by-page book reader GUI for TinyMaterialDesign.
 *
 * This header is optional: it needs TinyMaterialDesign, TinyGPU and TinyJPEG
 * in addition to TinyMobi.h. TinyMobi.h itself does not depend on them.
 */
#include <stdio.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "TinyFonts/Fonts/Lato10.h"
#include "TinyFonts/Fonts/Lato16.h"
#include "TinyFonts/Fonts/Lato32.h"
#include "TinyFonts/Fonts/Lato8.h"
#include "TinyFonts/Integrations/TinyGPU.h"
#include "TinyMaterialDesign.h"
#include "TinyMobi.h"
#include "TinyMobi/MobiReaderCover.h"
#include "TinyMobi/MobiReaderImage.h"

namespace tinymobi {

/**
 * @brief The book reader GUI for TinyMaterialDesign: a full screen JPEG cover,
 * then the book text one page at a time.
 *
 * - Cover: the JPEG cover of the book (if it has one), shown full screen before
 *   the first page. A tap or a swipe to the left opens the text.
 * - Text: a title bar with the book title, the page number (optional, see the
 *   settings) and a system menu (the menu icon in the title bar), and the text
 *   of the page. Images in the text are shown scaled to the text width, and a tap
 *   shows one full screen. Pages are turned by swiping left (next) and right
 *   (previous). The Previous and Next buttons are optional (see the settings).
 * - System menu: "Go to start", "Bookmarks" (add the shown page, jump to saved
 *   bookmarks), "Settings", "About" (author, publisher and language) and, if
 *   onClose is set, "Close". The table of contents is part of the text: its
 *   links jump to the chapters (see TextMode::PlainLinks).
 * - Settings: dark mode (on by default) and navigation buttons (off by default).
 *
 * The page text is read from the document with read() in plain text mode and
 * cut so that it fits the text area. Each page start is remembered, so
 * previous() works after next(). goTo() starts a new page sequence, e.g. at a
 * table of contents entry.
 *
 * Usage:
 * @code
 *   FileMobi<File> doc{StreamSource<File>(file)};
 *   MobiReaderApp<RGB565, FileMobi<File>> gui(DefaultBoard, doc);
 *
 * A landscape panel can show the UI rotated by 90 degrees (portrait). Wrap the
 * board in TinyGPU's LCDBoardRotated, which rotates in hardware if the display
 * driver supports it and in software otherwise:
 *   tinygpu::LCDBoardRotated rotated(DefaultBoard);
 *   MobiReaderApp<RGB565, FileMobi<File>> gui(rotated, doc);
 *
 *   void setup() {
 *     doc.open();   // the document must be open
 *     gui.begin();
 *   }
 *   void loop() { gui.update(); }
 * @endcode
 *
 * Limitations: the text pages are not scrolled. A single word that is taller
 * than the text area is clipped. At most kMaxPages pages can be reached.
 * Bookmarks are kept for the session only. Links that cannot be resolved are
 * shown as plain text.
 */
template <typename RGB_T = TINYMD_DEFAULT_RGB_T, typename Doc = MemoryMobi>
class MobiReaderApp {
 public:
  /// \brief Number of page starts that are remembered (pages reachable by previous()).
  static constexpr size_t kMaxPages = 4096;  // page starts remembered (8 bytes each)
  /// \brief Text read as the candidate for one page, before it is fitted to the screen.
  static constexpr size_t kPageBytes = 2000;

  /// \brief Called when the user chooses "Close" in the system menu. The item is
  /// only shown when this callback is set.
  std::function<void()> onClose;

  /// \brief Lists the entries of a folder for the open dialog. Set by the sketch.
  std::function<std::vector<tinymd::FileEntry>(const std::string& dir)> listFiles;

  /// \brief Opens the book at path into the document (e.g. file = SD.open(path);
  /// return doc.open() == Error::None). Set by the sketch.
  std::function<bool(const std::string& path)> openBook;

  /// \brief The folder the open dialog starts in.
  std::string libraryDir = "/";

  /// \brief The extensions of the books in the open dialog, e.g. ".mobi,.azw3".
  std::string bookExtensions = ".mobi,.azw,.azw3,.prc";

  /// \brief Creates the GUI on a board (e.g. DefaultBoard) for an open document,
  /// which must outlive the GUI. The cover is decoded in begin().
  MobiReaderApp(tinygpu::LCDBoard& board, Doc& doc)
      : doc_(doc),
        app_(board, darkTheme_),
        prev_(tinymd::Bounds(), "Previous", tinymd::ButtonVariant::kTonal),
        next_(tinymd::Bounds(), "Next", tinymd::ButtonVariant::kTonal) {}

  /// \brief Builds the GUI. If the document is open, its first page is shown;
  /// otherwise the open dialog is shown.
  void begin() {
    // the themes use the TinyFonts fonts (the themes must be set before the app starts)
    // small screens use the smaller Lato sizes: body text as the label font, titles as the body font
    const bool small = app_.width() < 200 || app_.height() < 240;
    const tinymd::Typography<RGB_T> fonts = small ? tinymd::Typography<RGB_T>{&labelFont_, &labelFont_, &bodyFont_, &headlineFont_}
                                                  : tinymd::Typography<RGB_T>{&labelFont_, &bodyFont_, &titleFont_, &headlineFont_};
    darkTheme_.typography = fonts;
    lightTheme_.typography = fonts;
    app_.screen().setTheme(dark_ ? darkTheme_ : lightTheme_);  // the screen has its own copy
    app_.begin();
    tinymd::Screen<RGB_T>& screen = app_.screen();
    text_.setRole(tinymd::TypographyRole::kBody);
    text_.onImage = [this](uint32_t record) { openImage(record); };
    imageView_.onClose = [this]() { app_.screen().dismissDialog(); };
    text_.onLink = [this](const std::string& target) {
      Position p;
      if (!doc_.resolveLink(target.c_str(), p)) return;
      if (history_.size() < kMaxHistory) history_.push_back(Jump{starts_[current_], coverShown_});
      goTo(p);
    };
    text_.onSwipe = [this](bool toNext) {
      if (toNext) {
        next();
      } else {
        previous();
      }
    };
    prev_.onClick = [this]() { previous(); };
    next_.onClick = [this]() { next(); };
    prev_.visible = buttonsVisible_;
    next_.visible = buttonsVisible_;
    buildTitleBar();
    buildMenu();
    buildSettings();
    cover_.view.onNext = [this]() { next(); };
    screen.addWidget(text_);
    screen.addWidget(prev_);
    screen.addWidget(next_);
    screen.addWidget(cover_.view);
    screen.addWidget(menuButton_);
    screen.addFixedWidget(fileDialog_.panel());
    fileDialog_.panel().visible = false;
    layout();
    if (doc_.isOpen()) {
      loadBook();
    } else {
      openFileDialog();
    }
  }

  /// \brief Shows the open dialog: the books of libraryDir. Tapping a book opens it.
  /// Cancel is shown when a book is open.
  void openFileDialog() {
    const int32_t w = app_.width();
    const int32_t h = app_.height();
    fileDialog_.listDirectory = listFiles;
    fileDialog_.onSelect = [this](const std::string& path) {
      closeFileDialog();
      if (openBook && openBook(path)) {
        loadBook();
      } else if (!doc_.isOpen()) {
        // the dialog rebuilds its rows, so it is shown again on the next update()
        reopenDialog_ = true;
      }
    };
    fileDialog_.onCancel = doc_.isOpen() ? std::function<void()>([this]() { closeFileDialog(); })
                                         : std::function<void()>();
    fileDialog_.setTheme(dark_ ? darkTheme_ : lightTheme_);
    fileDialog_.open(tinymd::Bounds(8, titleHeight() + 4, w - 16, h - titleHeight() - 12), "Open book", libraryDir,
                     bookExtensions);
    // a fixed widget, not a dialog: the title bar and its menu stay usable
    fileDialog_.panel().visible = true;
    app_.screen().invalidate();
  }

  /// \brief Hides the open dialog (e.g. when the menu is opened).
  void closeFileDialog() {
    fileDialog_.panel().visible = false;
    app_.screen().invalidate();
  }

  /// \brief Shows the book of the document: its title, cover, bookmarks
  /// and first page. Called after a book has been opened.
  void loadBook() {
    doc_.setTextMode(TextMode::PlainLinks);
    const Metadata& md = doc_.metadata();
    bookTitle_ = md.title[0] != 0 ? md.title : "Book";
    aboutText_ = std::string("Author: ") + md.author + "\nPublisher: " + md.publisher + "\nLanguage: " + md.language;
    coverShown_ = false;
    cover_.load(doc_, app_.width(), app_.height());
    bookmarks_.clear();
    history_.clear();
    imageCache_.clear();
    layout();
    goTo(Position(0, 0));
    if (cover_.has()) showCover();
  }

  /// \brief Call from loop(): handles touch input and redraws when needed.
  void update() {
    if (reopenDialog_) {
      reopenDialog_ = false;
      openFileDialog();
    }
    if (fileDialog_.poll()) app_.screen().invalidate();
    app_.update();
  }

  /// \brief Shows the next page, if there is one.
  void next() {
    if (coverShown_) {
      coverShown_ = false;
      showPage(0);
    } else if (hasNext()) {
      showPage(current_ + 1);
    }
  }

  /// \brief Shows the previous page, if there is one.
  void previous() {
    if (coverShown_) return;
    if (current_ == 0 && !history_.empty()) {
      // back from a followed link: the page it was followed from
      const Jump j = history_.back();
      history_.pop_back();
      goTo(j.start);
      if (j.cover) showCover();
    } else if (current_ == 0 && cover_.has()) {
      showCover();
    } else if (hasPrevious()) {
      showPage(current_ - 1);
    }
  }

  /// \brief Starts a new page sequence at position p (e.g. a table of contents entry).
  void goTo(Position p) {
    starts_.assign(1, p);
    coverShown_ = false;
    showPage(0);
  }

  /// \brief True if there is a page after the shown one.
  bool hasNext() const { return coverShown_ ? !starts_.empty() : current_ + 1 < starts_.size(); }
  /// \brief True if there is a page before the shown one.
  bool hasPrevious() const {
    return !coverShown_ && (current_ > 0 || cover_.has() || !history_.empty());
  }

  /// \brief True if the open book has a JPEG cover that could be decoded.
  bool hasCover() const { return cover_.has(); }

  /// \brief The text of the shown page (the text as drawn, without link markers).
  std::string pageText() const { return text_.text(); }

  /// \brief The number of the shown page, starting with 1.
  uint32_t pageNumber() const { return current_ + 1; }

  /// \brief Shows the cover page (if the book has a JPEG cover).
  void showCover() {
    coverShown_ = true;
    updateVisibility();
  }

  /// \brief True if the screen uses the dark theme (the default).
  bool isDark() const { return dark_; }

  /// \brief Switches between the dark and the light theme.
  void setDark(bool dark) {
    dark_ = dark;
    const MaterialTheme<RGB_T>& theme = dark ? darkTheme_ : lightTheme_;
    app_.screen().setTheme(theme);
    settings_.setTheme(theme);
    app_.screen().invalidate();
  }

  /// \brief True if the Previous and Next buttons are shown (the default is off:
  /// swipe the text instead).
  bool buttonsVisible() const { return buttonsVisible_; }

  /// \brief Shows or hides the page number in the title bar.
  void setPageNumberVisible(bool visible) {
    pageNumberVisible_ = visible;
    updateTitle();  // the title may use the space of the page number
    updateVisibility();
  }

  /// \brief True if the page number is shown in the title bar (the default).
  bool isPageNumberVisible() const { return pageNumberVisible_; }

  /// \brief Shows or hides the Previous and Next buttons.
  void setButtonsVisible(bool visible) {
    buttonsVisible_ = visible;
    updateVisibility();
    layout();
    app_.screen().invalidate();
  }

  /// \brief Opens the settings screen (also available in the system menu).
  void showSettings() {
    darkSwitch_.setValue(dark_);
    buttonsSwitch_.setValue(buttonsVisible_);
    app_.screen().presentDialog(settings_);
  }

  /// \brief Saves screenshots of the cover (if any), the next text pages and the
  /// settings screen. The sketch opens the file: store(name, write) must create
  /// the image file "name", call write(out) with its Print, close the file and
  /// return the result of write().
  void saveScreenshots(const std::function<bool(const char* name, const std::function<bool(Print&)>& write)>& store,
                       uint32_t pages = 3) {
    auto shot = [this, &store](const char* name) {
      app_.screen().invalidate();
      app_.update();
      return store(name, [this](Print& out) { return app_.saveScreenshot(out); });
    };
    goTo(Position(0, 0));
    if (cover_.has()) {
      showCover();
      shot("cover");
    }
    for (uint32_t i = 0; i < pages && hasNext(); ++i) {
      next();
      shot(("page" + std::to_string(i + 1)).c_str());
    }
    showSettings();
    shot("settings");
    app_.screen().dismissDialog();
  }

  /// \brief The TinyMaterialDesign app, e.g. for app().saveScreenshot(file).
  tinymd::App<RGB_T>& app() { return app_; }

 private:
  static constexpr int32_t kMargin = 12;
  static constexpr int32_t kTitleHeight = 56;
  static constexpr int32_t kNavHeight = 56;
  static constexpr int32_t kButtonWidth = 100;
  static constexpr int32_t kBadgeGap = 4;      // space between the page number and the menu icon
  static constexpr int32_t kRowHeight = 44;
  static constexpr size_t kMaxBookmarks = 32;

  // The sizes above are for the 240 x 320 screen. The layout scales them to the
  // screen: a smaller screen gets a smaller title bar, navigation bar, rows and
  // buttons, so the same code works from about 160 x 128 pixels up.
  int32_t titleHeight() const { return std::min<int32_t>(kTitleHeight, std::max<int32_t>(28, app_.height() / 5)); }
  int32_t navBarHeight() const { return std::min<int32_t>(kNavHeight, std::max<int32_t>(32, app_.height() / 5)); }
  int32_t rowHeight() const {
    return std::min<int32_t>(kRowHeight, std::max<int32_t>(24, (app_.height() - titleHeight()) / 6));
  }
  int32_t buttonWidth() const { return std::min<int32_t>(kButtonWidth, (app_.width() - 3 * kMargin) / 2); }

  /// \brief A page that a link was followed from: its start, and whether the
  /// cover was shown.
  struct Jump {
    Position start;
    bool cover = false;
  };
  static constexpr size_t kMaxHistory = 32;

  /// \brief A bookmark: the start of a page and its label.
  struct Bookmark {
    Position start;
    std::string label;
  };

  /// \brief A link in the shown page text: the characters [begin, end) and the
  /// target that Doc::resolveLink() accepts.
  struct PageLink {
    size_t begin = 0;
    size_t end = 0;
    std::string target;
  };

  /// \brief A character range [begin, end) of the shown page text.
  struct Span {
    size_t begin = 0;
    size_t end = 0;
  };

  /// \brief An image marker in the page text: its position and PalmDB record.
  struct ImageSpan {
    size_t begin = 0;
    uint32_t record = 0;
  };

  /// \brief An image of the page: its marker position, record and decoded picture
  /// (null if it could not be decoded: the line is left empty).
  struct PageImage {
    size_t begin = 0;
    uint32_t record = 0;
    std::shared_ptr<tinygpu::Surface<RGB_T>> pic;
  };

  /// \brief The full-screen view of an image: black, the picture centred, a tap
  /// closes it.
  class ImageView : public tinymd::Widget<RGB_T> {
   public:
    std::function<void()> onClose;

    void setImage(std::shared_ptr<tinygpu::Surface<RGB_T>> pic) { pic_ = std::move(pic); }

    void draw(tinygpu::ISurface<RGB_T>& target) override {
      target.fillRect(static_cast<size_t>(this->bounds.x), static_cast<size_t>(this->bounds.y),
                      static_cast<size_t>(this->bounds.w), static_cast<size_t>(this->bounds.h), RGB_T(0, 0, 0));
      if (!pic_) return;
      const int32_t x = this->bounds.x + (this->bounds.w - static_cast<int32_t>(pic_->width())) / 2;
      const int32_t y = this->bounds.y + (this->bounds.h - static_cast<int32_t>(pic_->height())) / 2;
      target.drawSprite(static_cast<size_t>(x < 0 ? 0 : x), static_cast<size_t>(y < 0 ? 0 : y), *pic_);
    }

    bool onGesture(const tinygpu::GestureEvent& event) override {
      if (event.phase != tinygpu::GesturePhase::kEnded || event.type != tinygpu::GestureType::kTap) return false;
      if (onClose) onClose();
      return true;
    }

   private:
    std::shared_ptr<tinygpu::Surface<RGB_T>> pic_;
  };

  /// \brief The text of a page. Swipes turn pages; a tap on a link calls onLink
  /// with its target. Headings are drawn with the title font, on their own lines.
  /// The layout is computed in one place, so drawing, the tap mapping and the
  /// page height agree.
  class PageTextBlock : public tinymd::TextBlock<RGB_T> {
   public:
    /// \brief Called with true for a swipe to the left (next page) and false for a
    /// swipe to the right (previous page).
    std::function<void(bool)> onSwipe;
    /// \brief Called with the target of a tapped link.
    std::function<void(const std::string&)> onLink;
    /// \brief Called with the PalmDB record of a tapped image.
    std::function<void(uint32_t)> onImage;

    /// \brief Shows text (already without markers) with its links, headings and images.
    void setPage(const std::string& text, std::vector<PageLink> links, std::vector<Span> headings,
                 std::vector<PageImage> images) {
      images_ = std::move(images);
      // a page that is mostly links (a table of contents) gets more space between the
      // lines, so a tap is less likely to hit the neighbouring entry
      size_t linkChars = 0;
      for (const PageLink& l : links) linkChars += l.end - l.begin;
      spacing_ = (!text.empty() && linkChars * 2 > text.size()) ? kLinkSpacing : kSpacing;
      links_ = std::move(links);
      headings_ = std::move(headings);
      this->setText(text.c_str());
    }

    /// \brief The height the page text needs at the given width.
    int32_t textHeight(int32_t width) const {
      std::vector<Line> lines;
      layoutLines(width, lines);
      return lines.empty() ? 0 : lines.back().y + lines.back().height - this->bounds.y;
    }

    void draw(tinygpu::ISurface<RGB_T>& target) override {
      const tinymd::MaterialTheme<RGB_T>& t = this->theme();
      const RGB_T background = t.colors.background;
      std::vector<Line> lines;
      layoutLines(this->bounds.w, lines);
      for (const Line& l : lines) {
        if (l.pic != nullptr) {
          const int32_t x = this->bounds.x + (this->bounds.w - static_cast<int32_t>(l.pic->width())) / 2;
          target.drawSprite(static_cast<size_t>(x < 0 ? 0 : x), static_cast<size_t>(l.y), *l.pic);
          continue;
        }
        if (l.text.empty()) continue;
        const RGB_T color = l.heading ? t.colors.onSurface : t.colors.onSurfaceVariant;
        l.font->drawText(target, static_cast<int16_t>(this->bounds.x), static_cast<int16_t>(l.y), l.text.c_str(),
                         color, background, false);
      }
    }

    bool onGesture(const tinygpu::GestureEvent& event) override {
      if (event.phase != tinygpu::GesturePhase::kEnded) return false;
      if (event.type == tinygpu::GestureType::kTap) {
        std::string target;
        if (onLink && linkAt(event.point.x, event.point.y, target)) {
          onLink(target);
          return true;
        }
        uint32_t record = 0;
        if (onImage && imageAt(event.point.x, event.point.y, record)) {
          onImage(record);
          return true;
        }
        return false;
      }
      if (event.type == tinygpu::GestureType::kSwipeLeft) {
        if (onSwipe) onSwipe(true);
        return true;
      }
      if (event.type == tinygpu::GestureType::kSwipeRight) {
        if (onSwipe) onSwipe(false);
        return true;
      }
      return false;
    }

   private:
    /// Line spacing (the same as tinymd::TextBlock's default).
    static constexpr int32_t kSpacing = 3;
    /// Line spacing on pages that are mostly links.
    static constexpr int32_t kLinkSpacing = 12;
    int32_t spacing_ = kSpacing;  // the spacing of the page shown

    struct Word {
      size_t begin;    // index in the text
      size_t end;
      size_t linePos;  // index in the line it is drawn in
    };

    struct Line {
      std::string text;          // the words joined by single spaces
      std::vector<Word> words;
      tinygpu::IFont<RGB_T>* font = nullptr;
      bool heading = false;
      const tinygpu::Surface<RGB_T>* pic = nullptr;  // an image line
      uint32_t record = 0;
      int32_t y = 0;             // top of the line
      int32_t height = 0;        // font height (or picture height) plus spacing
    };

    /// The marker character of an image in the page text (see TinyMobi.h).
    static constexpr char kImageMarker = '\x06';

    /// An image line: the picture of the image at position ps (blank if it has none).
    void imageLine(size_t ps, tinygpu::IFont<RGB_T>* font, std::vector<Line>& out) const {
      Line line;
      line.font = font;
      for (const PageImage& im : images_) {
        if (im.begin == ps && im.pic) {
          line.pic = im.pic.get();
          line.record = im.record;
          break;
        }
      }
      out.push_back(line);
    }

    bool isHeading(size_t index) const {
      for (const Span& h : headings_) {
        if (index >= h.begin && index < h.end) return true;
      }
      return false;
    }

    /// \brief Splits the text into paragraphs (at newlines) and wraps each one.
    void layoutLines(int32_t width, std::vector<Line>& out) const {
      out.clear();
      const std::string& text = this->text();
      const auto& theme = this->theme();
      tinygpu::IFont<RGB_T>* body = tinymd::fontForTypographyRole(tinymd::TypographyRole::kBody, theme);
      tinygpu::IFont<RGB_T>* title = tinymd::fontForTypographyRole(tinymd::TypographyRole::kTitle, theme);
      size_t ps = 0;
      for (;;) {
        size_t pe = text.find('\n', ps);
        if (pe == std::string::npos) pe = text.size();
        if (ps == pe && pe == text.size() && !text.empty()) break;  // no blank line after the last newline
        if (pe == ps + 1 && text[ps] == kImageMarker) {
          imageLine(ps, body, out);
        } else {
          const bool heading = isHeading(ps);
          wrapParagraph(text, ps, pe, width, heading ? title : body, heading, out);
        }
        if (pe >= text.size()) break;
        ps = pe + 1;
      }
      int32_t y = this->bounds.y;
      for (Line& l : out) {
        l.y = y;
        l.height = (l.pic != nullptr ? static_cast<int32_t>(l.pic->height())
                                     : static_cast<int32_t>(l.font->getHeight(1))) + spacing_;
        y += l.height;
      }
    }

    void wrapParagraph(const std::string& text, size_t ps, size_t pe, int32_t width, tinygpu::IFont<RGB_T>* font,
                       bool heading, std::vector<Line>& out) const {
      const size_t before = out.size();
      Line line;
      line.font = font;
      line.heading = heading;
      auto fits = [&](const std::string& s) { return static_cast<int32_t>(font->measureTextWidth(s.c_str())) <= width; };
      auto endLine = [&]() {
        out.push_back(line);
        line = Line();
        line.font = font;
        line.heading = heading;
      };
      size_t wb = std::string::npos;
      for (size_t i = ps; i <= pe; ++i) {
        const char c = i < pe ? text[i] : ' ';
        if (c != ' ') {
          if (wb == std::string::npos) wb = i;
          continue;
        }
        if (wb == std::string::npos) continue;
        const std::string word = text.substr(wb, i - wb);
        const std::string candidate = line.text.empty() ? word : line.text + " " + word;
        if (!line.text.empty() && !fits(candidate)) {
          endLine();
          line.text = word;
          line.words.push_back(Word{wb, i, 0});
        } else {
          line.words.push_back(Word{wb, i, line.text.empty() ? 0 : line.text.size() + 1});
          line.text = candidate;
        }
        wb = std::string::npos;
      }
      if (!line.text.empty()) endLine();
      if (out.size() == before) endLine();  // an empty paragraph is a blank line
    }

    /// \brief Finds the image under the point (x, y), if any.
    bool imageAt(int32_t x, int32_t y, uint32_t& record) const {
      if (images_.empty()) return false;
      std::vector<Line> lines;
      layoutLines(this->bounds.w, lines);
      for (const Line& l : lines) {
        const int32_t top = l.y - spacing_ / 2;
        const int32_t bottom = l.y + l.height - spacing_ / 2;
        if (y < top || y >= bottom) continue;
        if (l.pic == nullptr) return false;
        record = l.record;
        return x >= this->bounds.x && x < this->bounds.x + this->bounds.w;
      }
      return false;
    }

    /// \brief Finds the link under the point (x, y), if any.
    bool linkAt(int32_t x, int32_t y, std::string& target) const {
      if (links_.empty()) return false;
      std::vector<Line> lines;
      layoutLines(this->bounds.w, lines);
      const std::string& text = this->text();
      for (const Line& l : lines) {
        // the space between two lines is shared: each line takes half of it on either side
        const int32_t top = l.y - spacing_ / 2;
        const int32_t bottom = l.y + l.height - spacing_ / 2;
        if (y < top || y >= bottom) continue;
        for (const Word& w : l.words) {
          const std::string word = text.substr(w.begin, w.end - w.begin);
          const int32_t x0 = this->bounds.x + static_cast<int32_t>(l.font->measureTextWidth(l.text.substr(0, w.linePos).c_str()));
          const int32_t x1 = x0 + static_cast<int32_t>(l.font->measureTextWidth(word.c_str()));
          if (x < x0 || x >= x1) continue;
          for (const PageLink& k : links_) {
            if (w.begin >= k.begin && w.begin < k.end) {
              target = k.target;
              return true;
            }
          }
          return false;
        }
        return false;
      }
      return false;
    }

    std::vector<PageLink> links_;
    std::vector<Span> headings_;
    std::vector<PageImage> images_;
  };

  /// \brief The place of the page number: left of the menu icon, in the title bar.
  /// The page number is right-aligned: its box ends just left of the menu icon, and
  /// is as wide as the number (see updateTitle()).
  tinymd::Bounds pageLabelBounds(int32_t textWidth) const {
    const tinymd::Bounds menu = appBar_.trailingRect();
    return tinymd::Bounds(menu.x - kBadgeGap - textWidth, appBar_.bounds.y, textWidth, titleHeight());
  }

  /// \brief True if the page number is shown (the setting, and a book is open).
  bool pageNumberShown() const { return pageNumberVisible_ && doc_.isOpen(); }

  /// \brief The title bar with the book title, the page number and the menu icon.
  void buildTitleBar() {
    appBar_ = tinymd::AppBar<RGB_T>(tinymd::Bounds(0, 0, app_.width(), titleHeight()), "");
    menuButton_ = tinymd::IconButton<RGB_T>(appBar_.trailingRect(), tinymd::drawMenu<RGB_T>);
    menuButton_.onClick = [this]() {
      closeFileDialog();
      openMenu();
    };
    appBar_.trailing = &menuButton_;
    app_.screen().addFixedWidget(appBar_);
    // the page number, left of the menu icon (the title is cut before it)
    pageLabel_ = tinymd::Label<RGB_T>(pageLabelBounds(0), "", tinymd::TypographyRole::kTitle);
    app_.screen().addFixedWidget(pageLabel_);
    updateTitle();
  }

  /// \brief The system menu.
  void buildMenu() {
    const int32_t w = app_.width();
    const int32_t h = app_.height();
    const int32_t menuW = std::min<int32_t>(170, w - 8);
    const int32_t itemH = std::min<int32_t>(32, std::max<int32_t>(20, (h - titleHeight()) / 7));
    menu_ = tinymd::Menu<RGB_T>(tinymd::Bounds(w - menuW - 4, titleHeight(), menuW, 7 * itemH));

    openItem_ = tinymd::ListItem<RGB_T>(menu_.itemRect(0, itemH), "Open book");
    openItem_.onClick = [this]() {
      app_.screen().dismissDialog();
      openFileDialog();
    };
    menu_.addItem(openItem_);

    startItem_ = tinymd::ListItem<RGB_T>(menu_.itemRect(1, itemH), "Go to start");
    startItem_.onClick = [this]() {
      app_.screen().dismissDialog();
      goTo(Position(0, 0));
      if (cover_.has()) showCover();
    };
    menu_.addItem(startItem_);

    bookmarksItem_ = tinymd::ListItem<RGB_T>(menu_.itemRect(2, itemH), "Bookmarks");
    bookmarksItem_.onClick = [this]() {
      app_.screen().dismissDialog();
      openBookmarks();
    };
    menu_.addItem(bookmarksItem_);

    settingsItem_ = tinymd::ListItem<RGB_T>(menu_.itemRect(3, itemH), "Settings");
    settingsItem_.onClick = [this]() {
      app_.screen().dismissDialog();
      showSettings();
    };
    menu_.addItem(settingsItem_);

    aboutItem_ = tinymd::ListItem<RGB_T>(menu_.itemRect(4, itemH), "About");
    aboutItem_.onClick = [this]() {
      app_.screen().dismissDialog();
      openAbout();
    };
    menu_.addItem(aboutItem_);

    closeItem_ = tinymd::ListItem<RGB_T>(menu_.itemRect(5, itemH), "Close");
    closeItem_.onClick = [this]() {
      app_.screen().dismissDialog();
      if (onClose) onClose();
    };
    menu_.addItem(closeItem_);
    menu_.onOutsideTap = [this]() { app_.screen().dismissDialog(); };
  }

  /// \brief The About dialog with the author, publisher and language of the book.
  void openAbout() {
    const int32_t w = app_.width();
    const int32_t h = app_.height();
    const int32_t dialogH = std::min<int32_t>(200, h - 16);
    // the page that is shown now (the dialog copies the text)
    const std::string page = coverShown_ ? std::string("Cover") : std::to_string(pageNumber());
    const std::string text = aboutText_ + "\nPage: " + page;
    about_ = tinymd::Dialog<RGB_T>(tinymd::Bounds(16, (h - dialogH) / 2, w - 32, dialogH), bookTitle_.c_str(),
                                   text.c_str());
    aboutOk_ = tinymd::Button<RGB_T>(about_.actionRect(0, 1), "OK");
    aboutOk_.onClick = [this]() { app_.screen().dismissDialog(); };
    about_.addAction(aboutOk_);
    app_.screen().presentDialog(about_);
  }

  /// \brief The settings screen: a panel with the dark mode and the navigation
  /// button switches, and a Done button.
  void buildSettings() {
    const int32_t w = app_.width();
    const int32_t h = app_.height();
    const int32_t panelH = std::min<int32_t>(300, h - 16);
    // the rows are placed for a 300 pixel panel and scaled to the panel height
    auto row = [panelH](int32_t y) { return y * panelH / 300; };
    // narrow screens: a smaller margin, switches and label column, so the labels fit
    const bool narrow = w < 200;
    const int32_t pad = narrow ? 4 : 12;
    const int32_t switchW = narrow ? 44 : 52, switchH = narrow ? 26 : 32;
    const tinymd::Bounds panel(pad, (h - panelH) / 2, w - 2 * pad, panelH);
    settings_ = tinymd::Container<RGB_T>(panel);
    settingsBackground_ = tinymd::Card<RGB_T>(panel);
    settings_.addChild(settingsBackground_);

    const int32_t rowX = panel.x + (narrow ? 8 : 16);
    const int32_t labelW = panel.w - (narrow ? 16 : 32) - switchW - 8;
    settingsTitle_ = tinymd::Label<RGB_T>(tinymd::Bounds(rowX, panel.y + row(12), panel.w - 32, 32), "Settings",
                                          tinymd::TypographyRole::kTitle);
    darkLabel_ = tinymd::Label<RGB_T>(tinymd::Bounds(rowX, panel.y + row(60), labelW, 32), "Dark mode");
    darkSwitch_ = tinymd::Switch<RGB_T>(tinymd::Bounds(rowX + labelW + 8, panel.y + row(60), switchW, switchH), dark_);
    darkSwitch_.onChange = [this](bool on) { setDark(on); };
    buttonsLabel_ =
        tinymd::Label<RGB_T>(tinymd::Bounds(rowX, panel.y + row(112), labelW, 32), "Navigation buttons");
    buttonsSwitch_ =
        tinymd::Switch<RGB_T>(tinymd::Bounds(rowX + labelW + 8, panel.y + row(112), switchW, switchH), buttonsVisible_);
    buttonsSwitch_.onChange = [this](bool on) { setButtonsVisible(on); };
    pageNumberRowLabel_ =
        tinymd::Label<RGB_T>(tinymd::Bounds(rowX, panel.y + row(164), labelW, 32), "Page number");
    pageNumberSwitch_ =
        tinymd::Switch<RGB_T>(tinymd::Bounds(rowX + labelW + 8, panel.y + row(164), switchW, switchH), pageNumberVisible_);
    pageNumberSwitch_.onChange = [this](bool on) { setPageNumberVisible(on); };
    // the hint stays above the Done button (see below)
    const int32_t hintY = std::min<int32_t>(panel.y + row(208), panel.bottom() - (narrow ? 8 : 12) - (narrow ? 32 : 40) - 28);
    settingsHint_ = tinymd::Label<RGB_T>(tinymd::Bounds(rowX, hintY, panel.w - 32, 24),
                                         "Swipe the text to turn pages.",
                                         tinymd::TypographyRole::kLabel);
    // on a very short panel the hint would cover the last row: it is left out
    settingsHint_.visible = hintY >= panel.y + row(164) + 40;
    // the Done button sits in the bottom right corner of the panel, and the hint
    // above it ends before the button
    const int32_t doneW = std::min<int32_t>(100, panel.w / 2), doneH = narrow ? 32 : 40;
    const int32_t doneX = panel.right() - (narrow ? 8 : 16) - doneW;
    const int32_t doneY = panel.bottom() - (narrow ? 8 : 12) - doneH;
    settingsDone_ = tinymd::Button<RGB_T>(tinymd::Bounds(doneX, doneY, doneW, doneH), "Done", tinymd::ButtonVariant::kText);
    settingsDone_.onClick = [this]() { app_.screen().dismissDialog(); };

    settings_.addChild(settingsTitle_);
    settings_.addChild(darkLabel_);
    settings_.addChild(darkSwitch_);
    settings_.addChild(buttonsLabel_);
    settings_.addChild(buttonsSwitch_);
    settings_.addChild(pageNumberRowLabel_);
    settings_.addChild(pageNumberSwitch_);
    settings_.addChild(settingsHint_);
    settings_.addChild(settingsDone_);
  }

  /// \brief The bookmarks panel: a row to bookmark the shown page, and one row per
  /// bookmark. The bookmarks are kept for this session.
  void openBookmarks() {
    const int32_t w = app_.width();
    const int32_t h = app_.height();
    const tinymd::Bounds panel(8, titleHeight() + 4, w - 16, h - titleHeight() - 12);
    const tinymd::Bounds row0(panel.x + 8, panel.y + 8, panel.w - 16, rowHeight());
    bookmarkRows_.clear();
    bookmarksPanel_ = tinymd::Container<RGB_T>(panel);
    bookmarksBackground_ = tinymd::Card<RGB_T>(panel);
    bookmarksPanel_.setTheme(dark_ ? darkTheme_ : lightTheme_);
    bookmarksPanel_.addChild(bookmarksBackground_);

    auto addRow = [this](const tinymd::Bounds& rect, const std::string& label, std::function<void()> onClick) {
      std::unique_ptr<tinymd::ListItem<RGB_T>> row(new tinymd::ListItem<RGB_T>(rect, label.c_str()));
      row->onClick = std::move(onClick);
      bookmarksPanel_.addChild(*row);
      bookmarkRows_.push_back(std::move(row));
    };

    addRow(row0, "Close", [this]() { app_.screen().dismissDialog(); });
    int32_t y = row0.y + rowHeight() + 4;
    addRow(tinymd::Bounds(row0.x, y, row0.w, rowHeight()), "Add bookmark - page " + std::to_string(pageNumber()),
           [this]() {
             app_.screen().dismissDialog();
             if (bookmarks_.size() >= kMaxBookmarks) return;
             Bookmark b;
             b.start = coverShown_ ? starts_[0] : starts_[current_];
             b.label = "Page " + std::to_string(current_ + 1);
             bookmarks_.push_back(b);
           });
    y += rowHeight() + 4;
    for (size_t i = 0; i < bookmarks_.size(); ++i) {
      addRow(tinymd::Bounds(row0.x, y, row0.w, rowHeight()), bookmarks_[i].label, [this, i]() {
        app_.screen().dismissDialog();
        goTo(bookmarks_[i].start);
      });
      y += rowHeight();
    }
    app_.screen().presentDialog(bookmarksPanel_);
  }

  /// \brief Opens the system menu; "Close" is only listed if onClose is set.
  void openMenu() {
    closeItem_.visible = static_cast<bool>(onClose);
    // all six slots are kept, so a hidden "Close" does not leave a scroll bar
    menu_.bounds.h = 6 * startItem_.bounds.h;
    app_.screen().presentDialog(menu_);
  }

  /// \brief Shows the (shortened) book title and the page number in the title bar.
  void updateTitle() {
    if (!doc_.isOpen()) {  // no book: the application name, and no page number
      appBar_.setTitle("TinyMOBI");
      pageLabel_.setText("");
      return;
    }
    tinygpu::IFont<RGB_T>& font = *(dark_ ? darkTheme_ : lightTheme_).typography.title;
    auto width = [&](const std::string& t) { return static_cast<int32_t>(font.measureTextWidth(t.c_str())); };
    // the page number, right-aligned next to the menu icon
    const std::string number = std::to_string(current_ + 1);
    pageLabel_.setText(number.c_str());
    pageLabel_.bounds = pageLabelBounds(width(number));
    // the title is cut to the width between its start and the page number (or the menu)
    const int32_t start = appBar_.bounds.x + 16;
    const int32_t end = (pageNumberShown() ? pageLabel_.bounds.x : menuButton_.bounds.x) - kBadgeGap;
    const int32_t avail = end - start;
    std::string shortTitle = bookTitle_;
    if (width(shortTitle) > avail) {
      while (!shortTitle.empty() && width(shortTitle + "...") > avail) shortTitle.pop_back();
      shortTitle += "...";
    }
    appBar_.setTitle(shortTitle.c_str());
  }

  /// \brief The title bar at the top, the text area and, if enabled, the navigation
  /// buttons at the bottom.
  void layout() {
    const int32_t w = app_.width();
    const int32_t h = app_.height();
    appBar_.bounds = tinymd::Bounds(0, 0, w, titleHeight());
    menuButton_.bounds = appBar_.trailingRect();
    text_.bounds = textBounds();
    pageLabel_.bounds = pageLabelBounds(pageLabel_.bounds.w);
    const int32_t buttonY = h - navBarHeight() + 8;
    prev_.bounds = tinymd::Bounds(kMargin, buttonY, buttonWidth(), 40);
    next_.bounds = tinymd::Bounds(w - kMargin - buttonWidth(), buttonY, buttonWidth(), 40);
  }

  /// \brief The area for the text of the shown page: below the title bar, and
  /// below the cover on the first page. The bottom is above the navigation
  /// buttons when they are shown.
  tinymd::Bounds textBounds() const {
    const int32_t w = app_.width();
    const int32_t h = app_.height();
    const int32_t top = titleHeight() + kMargin;
    const int32_t bottom = h - navHeight() - kMargin;
    return tinymd::Bounds(kMargin, top, w - 2 * kMargin, bottom - top);
  }

  /// \brief The height of the navigation buttons at the bottom (0 if they are hidden).
  int32_t navHeight() const { return buttonsVisible_ ? navBarHeight() : 0; }


  /// \brief Shows the cover page or the text page, with the bars that belong to it.
  void updateVisibility() {
    const bool cover = coverShown_ && cover_.has();
    cover_.view.visible = cover;
    text_.visible = !cover;
    appBar_.visible = !cover;
    menuButton_.visible = !cover;
    pageLabel_.visible = !cover && pageNumberShown();
    prev_.visible = buttonsVisible_ && !cover;
    next_.visible = buttonsVisible_ && !cover;
    app_.screen().invalidate();
  }

  /// \brief Shows a page whose start is starts_[index] (known).
  void showPage(uint32_t index) {
    current_ = index;
    text_.bounds = textBounds();
    std::string text;
    bool eof = readCandidate(starts_[index], text);
    bool cut = false;
    if (!eof) cut = cutAtWord(text) || cut;
    cut = fitText(text) || cut;
    // the page ends where its text ends
    Position end = endOf(starts_[index], text.size());
    if (index + 1 == starts_.size() && (!eof || cut) && starts_.size() < kMaxPages) {
      starts_.push_back(end);
    }
    setText(text);
    updateTitle();
    prev_.enabled = hasPrevious();
    next_.enabled = hasNext();
    updateVisibility();
  }

  /// \brief Reads up to kPageBytes of text from start. Returns true if the end of
  /// the book was reached.
  bool readCandidate(Position start, std::string& text) {
    text.clear();
    if (doc_.seek(start) != Error::None) return true;
    uint8_t b = 0;
    while (text.size() < kPageBytes) {
      if (doc_.read(&b, 1) == 0) return true;
      text.push_back(static_cast<char>(b));
    }
    return doc_.read(&b, 1) == 0;  // true if nothing follows
  }

  /// \brief Position after the first n bytes of the text that starts at start.
  Position endOf(Position start, size_t n) {
    if (doc_.seek(start) != Error::None) return start;
    uint8_t b = 0;
    for (size_t i = 0; i < n; ++i) {
      if (doc_.read(&b, 1) == 0) break;
    }
    return doc_.position();
  }

  /// \brief Cuts the text after its last word. Returns true if something was cut.
  static bool cutAtWord(std::string& text) {
    size_t sp = text.find_last_of(" \n\t\r");
    if (sp == std::string::npos || sp == 0) return false;
    text.resize(sp);
    return true;
  }

  /// \brief Removes the link markers from text (see TextMode::PlainLinks) and
  /// returns the links with their character positions in the result.
  /// \brief Splits the page text into the visible text, its links and headings. The
  /// newlines at the top of the page are dropped (no empty line before the first
  /// heading), and a first heading equal to hiddenHeading (the book title, shown in
  /// the title bar) is removed.
  static void stripLinks(const std::string& text, std::string& out, std::vector<PageLink>& links,
                         std::vector<Span>& headings, std::vector<ImageSpan>& images,
                         const std::string& hiddenHeading) {
    out.clear();
    links.clear();
    headings.clear();
    images.clear();
    PageLink cur;
    size_t headBegin = 0;
    bool inHead = false, inTarget = false, inLink = false;
    bool inImage = false;
    uint32_t imageRecord = 0;
    for (char ch : text) {
      const uint8_t b = static_cast<uint8_t>(ch);
      if (b == kHeadBegin) {
        headBegin = out.size();
        inHead = true;
      } else if (b == kHeadEnd) {
        if (inHead && out.size() > headBegin) headings.push_back(Span{headBegin, out.size()});
        inHead = false;
      } else if (b == kImageBegin) {
        inImage = true;
        imageRecord = 0;
      } else if (inImage) {
        if (b == kImageEnd) {
          inImage = false;
          images.push_back(ImageSpan{out.size(), imageRecord});
          out.push_back('\x06');  // the image placeholder (PageTextBlock draws the picture)
        } else if (b >= '0' && b <= '9') {
          imageRecord = imageRecord * 10 + (b - '0');
        }
      } else if (b == kLinkBegin) {
        cur = PageLink();
        cur.begin = out.size();
        cur.target.clear();
        inTarget = inLink = true;
      } else if (inTarget) {
        if (b == kLinkTarget) inTarget = false;
        else cur.target.push_back(ch);
      } else if (b == kLinkEnd) {
        if (inLink && cur.end > cur.begin) links.push_back(cur);
        inLink = false;
      } else {
        if (b == '\n' && out.empty()) continue;  // no empty line at the top of the page
        out.push_back(ch);
        if (inLink) cur.end = out.size();
      }
    }
    // a link or heading cut by the end of the page: keep its visible part
    if (inLink && !inTarget && cur.end > cur.begin) links.push_back(cur);
    if (inHead && out.size() > headBegin) headings.push_back(Span{headBegin, out.size()});

    // a first heading that is the book title is not shown again (the title bar has it)
    if (!headings.empty() && headings[0].begin == 0 && !hiddenHeading.empty() &&
        trimmed(out.substr(0, headings[0].end)) == trimmed(hiddenHeading)) {
      size_t cut = headings[0].end;
      while (cut < out.size() && out[cut] == '\n') ++cut;
      out.erase(0, cut);
      headings.erase(headings.begin());
      for (Span& h : headings) {
        h.begin -= cut;
        h.end -= cut;
      }
      std::vector<PageLink> kept;
      for (PageLink& l : links) {
        if (l.begin < cut) continue;
        l.begin -= cut;
        l.end -= cut;
        kept.push_back(l);
      }
      links.swap(kept);
      std::vector<ImageSpan> keptImages;
      for (ImageSpan& im : images) {
        if (im.begin < cut) continue;
        im.begin -= cut;
        keptImages.push_back(im);
      }
      images.swap(keptImages);
    }
  }

  static std::string trimmed(const std::string& t) {
    const size_t b = t.find_first_not_of(" \n\t\r");
    if (b == std::string::npos) return std::string();
    const size_t e = t.find_last_not_of(" \n\t\r");
    return t.substr(b, e - b + 1);
  }

  /// \brief Cuts whole words until the text fits the text area. Returns true if
  /// something was cut. The text is measured without its link markers.
  bool fitText(std::string& text) {
    bool cut = false;
    setText(text);  // the text block gets the headings and images too: its height depends on them
    while (!text.empty() && text_.textHeight(text_.bounds.w) > text_.bounds.h) {
      if (text.size() < 2) break;
      size_t sp = text.find_last_of(" \n\t\r", text.size() - 2);
      if (sp == std::string::npos || sp == 0) break;
      text.resize(sp);
      cut = true;
      setText(text);
    }
    return cut;
  }

  /// \brief Shows the page text (raw, with markers): the links, headings and images
  /// are set, and each image is decoded to fit the text area.
  void setText(const std::string& raw) {
    std::string shown;
    std::vector<PageLink> links;
    std::vector<Span> headings;
    std::vector<ImageSpan> spans;
    stripLinks(raw, shown, links, headings, spans, bookTitle_);
    std::vector<PageImage> images;
    for (const ImageSpan& span : spans) {
      images.push_back(PageImage{span.begin, span.record,
                                 imageFor(span.record, text_.bounds.w, text_.bounds.h)});
    }
    text_.setPage(shown, std::move(links), std::move(headings), std::move(images));
  }

  /// \brief The decoded picture of an image record, fitted to maxW x maxH. The last
  /// few results are kept, so turning back to a page does not decode again.
  std::shared_ptr<tinygpu::Surface<RGB_T>> imageFor(uint32_t record, size_t maxW, size_t maxH) {
    for (const CachedImage& c : imageCache_) {
      if (c.record == record && c.maxW == maxW && c.maxH == maxH) return c.pic;
    }
    std::shared_ptr<tinygpu::Surface<RGB_T>> pic;
    uint32_t len = 0;
    if (record != kNoRecord && doc_.recordLength(record, len) == Error::None && len > 0) {
      std::vector<uint8_t> bytes(len);
      size_t n = 0;
      if (doc_.readRecord(record, bytes.data(), bytes.size(), n) == Error::None) {
        pic = decodeImageFit<RGB_T>(bytes.data(), n, maxW, maxH);
      }
    }
    if (imageCache_.size() >= kImageCache) imageCache_.erase(imageCache_.begin());
    imageCache_.push_back(CachedImage{record, maxW, maxH, pic});
    return pic;
  }

  /// \brief Shows an image full screen; a tap closes it.
  void openImage(uint32_t record) {
    const size_t w = app_.width(), h = app_.height();
    std::shared_ptr<tinygpu::Surface<RGB_T>> pic = imageFor(record, w, h);
    if (!pic) return;
    imageView_.setImage(pic);
    imageView_.bounds = tinymd::Bounds(0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h));
    app_.screen().presentDialog(imageView_);
  }

  Doc& doc_;
  // the themes must outlive the app: the screen keeps a pointer to them
  // the text is drawn with TinyFonts (Latin-1 characters, unlike the bitmap fonts)
  TinyGPUFont<RGB_T> labelFont_{Lato8};
  TinyGPUFont<RGB_T> bodyFont_{Lato10};
  TinyGPUFont<RGB_T> titleFont_{Lato16};
  TinyGPUFont<RGB_T> headlineFont_{Lato32};
  tinymd::MaterialTheme<RGB_T> darkTheme_ = tinymd::defaultDarkTheme<RGB_T>();
  tinymd::MaterialTheme<RGB_T> lightTheme_ = tinymd::defaultTheme<RGB_T>();
  tinymd::App<RGB_T> app_;
  PageTextBlock text_;
  ImageView imageView_;
  tinymd::Button<RGB_T> prev_;
  tinymd::Button<RGB_T> next_;
  tinymd::AppBar<RGB_T> appBar_;
  tinymd::Label<RGB_T> pageLabel_;
  tinymd::IconButton<RGB_T> menuButton_;
  tinymd::Menu<RGB_T> menu_;
  tinymd::ListItem<RGB_T> startItem_;
  tinymd::ListItem<RGB_T> openItem_;
  tinymd::FileDialog<RGB_T> fileDialog_;
  tinymd::ListItem<RGB_T> bookmarksItem_;
  tinymd::Container<RGB_T> bookmarksPanel_;
  tinymd::Card<RGB_T> bookmarksBackground_;
  std::vector<std::unique_ptr<tinymd::ListItem<RGB_T>>> bookmarkRows_;
  struct CachedImage {
    uint32_t record;
    size_t maxW;
    size_t maxH;
    std::shared_ptr<tinygpu::Surface<RGB_T>> pic;
  };
  static constexpr size_t kImageCache = 4;
  std::vector<CachedImage> imageCache_;
  std::vector<Bookmark> bookmarks_;
  std::vector<Jump> history_;  // the pages that links were followed from
  tinymd::ListItem<RGB_T> settingsItem_;
  tinymd::ListItem<RGB_T> aboutItem_;
  tinymd::ListItem<RGB_T> closeItem_;
  tinymd::Dialog<RGB_T> about_;
  tinymd::Button<RGB_T> aboutOk_;
  tinymd::Container<RGB_T> settings_;
  tinymd::Card<RGB_T> settingsBackground_;
  tinymd::Label<RGB_T> settingsTitle_;
  tinymd::Label<RGB_T> darkLabel_;
  tinymd::Switch<RGB_T> darkSwitch_;
  tinymd::Label<RGB_T> buttonsLabel_;
  tinymd::Switch<RGB_T> buttonsSwitch_;
  tinymd::Label<RGB_T> pageNumberRowLabel_;
  tinymd::Switch<RGB_T> pageNumberSwitch_;
  tinymd::Label<RGB_T> settingsHint_;
  tinymd::Button<RGB_T> settingsDone_;
  MobiCover<RGB_T, Doc> cover_;
  bool coverShown_ = false;
  bool reopenDialog_ = false;
  bool dark_ = true;
  bool buttonsVisible_ = false;
  bool pageNumberVisible_ = true;
  std::string bookTitle_;
  std::string aboutText_;
  std::vector<Position> starts_;  // page starts known so far: starts_[0] is the first page
  uint32_t current_ = 0;  // index of the shown page
};

}  // namespace tinymobi
