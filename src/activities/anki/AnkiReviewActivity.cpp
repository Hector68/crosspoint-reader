#include "AnkiReviewActivity.h"

#include <Arduino.h>  // ESP.getFreeHeap for the MEM ledger
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Lines of wrapped text shown per side of a card. Sized together with
// AnkiCardStore::MAX_FIELD_BYTES: a field that long always fits.
constexpr int CARD_TEXT_LINES = 5;
constexpr int TEXT_LINE_GAP = 2;
// Blank space between the card text and the picture. The picture is the last
// thing in the block, so without it the artwork sits flush against the word.
constexpr int IMAGE_TOP_GAP = 8;
}  // namespace

AnkiReviewActivity::AnkiReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, AnkiDeckRef deck)
    : Activity("AnkiReview", renderer, mappedInput), deck_(std::move(deck)) {}

void AnkiReviewActivity::onEnter() {
  Activity::onEnter();

  nowEpoch_ = static_cast<uint32_t>(time(nullptr));

  if (!store_.open(deck_.folder)) {
    missingDeck_ = true;
    draw();
    return;
  }

  buildQueue(true);
  if (!queue_.empty()) showCard(queue_[0]);
  // Heap ledger for field reports: this is the peak for the screen, with the
  // deck's line offsets (4 bytes per card) and the due queue both resident.
  LOG_INF("MEM", "anki review enter: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  draw();
}

void AnkiReviewActivity::onExit() {
  Activity::onExit();
  // Same courtesy BmpViewerActivity pays the next screen: the card face we drew
  // must not be mistaken for the previous screen's content.
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void AnkiReviewActivity::buildQueue(const bool dueOnly) {
  dueOnly_ = dueOnly;
  queue_.clear();
  queuePos_ = 0;
  current_ = SIZE_MAX;
  knownCount_ = 0;
  flipped_ = false;

  buildReviewQueue(store_, nowEpoch_, dueOnly, queue_);
  if (!queue_.empty()) showCard(queue_[0]);
}

void AnkiReviewActivity::showCard(const size_t index) {
  reviewedAnything_ = true;
  current_ = index;
  flipped_ = false;
  card_ = AnkiCard{};
  if (!store_.readCard(index, card_)) {
    LOG_ERR("ANKI", "Cannot read card %zu of deck %s", index, deck_.folder.c_str());
    card_.word = tr(STR_CARD_UNREADABLE);
  }
}

void AnkiReviewActivity::answer(const AnkiGrade grade) {
  if (current_ == SIZE_MAX) return;

  if (!store_.recordGrade(current_, grade, nowEpoch_)) {
    LOG_ERR("ANKI", "Could not persist progress for card %zu of %s", current_, deck_.folder.c_str());
  }

  if (grade == AnkiGrade::Good) {
    ++knownCount_;
  } else if (queue_.size() < AnkiCardStore::MAX_CARDS_PER_DECK) {
    // Not known yet: show it again at the end of this session.
    queue_.push_back(current_);
  }

  if (queuePos_ + 1 < queue_.size()) {
    ++queuePos_;
    showCard(queue_[queuePos_]);
  } else {
    current_ = SIZE_MAX;  // session finished
  }
  draw();
}

void AnkiReviewActivity::loop() {
  mappedInput.update();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (current_ == SIZE_MAX) {
    // Session over, or nothing was due: Confirm reviews the whole deck again.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      buildQueue(false);
      draw();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    flipped_ = !flipped_;
    draw();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    answer(AnkiGrade::Good);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    answer(AnkiGrade::Again);
    return;
  }
}

void AnkiReviewActivity::draw() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();

  char subtitle[64];
  subtitle[0] = '\0';
  if (current_ != SIZE_MAX && !queue_.empty()) {
    snprintf(subtitle, sizeof(subtitle), tr(STR_CARD_N_OF_M), static_cast<unsigned>(queuePos_ + 1),
             static_cast<unsigned>(queue_.size()));
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, deck_.title.c_str(),
                 subtitle[0] ? subtitle : nullptr, /*backButton=*/mappedInput.hasTouch());

  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int textWidth = safe.width;
  int y = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = pageHeight - metrics.buttonHintsHeight;

  if (missingDeck_) {
    renderer.drawCenteredText(UI_10_FONT_ID, (y + bottom) / 2, tr(STR_DECK_NOT_FOUND));
  } else if (current_ == SIZE_MAX) {
    // tr() is a macro that qualifies its argument as StrId::<id>, so a dynamic
    // choice has to go through I18N.get() with an explicit StrId qualifier.
    renderer.drawCenteredText(UI_10_FONT_ID, (y + bottom) / 2,
                              I18N.get(reviewedAnything_ ? StrId::STR_REVIEW_COMPLETE : StrId::STR_NOTHING_DUE));
  } else {
    // Side A (before the flip): Word (large) + Transcription (smaller) + Image.
    // Side B (after the flip): Translate + Image. The main line is pinned to
    // the top of the content area, so the word — and, after the flip, the
    // translation — always starts in the same place; the pronunciation hint
    // and the picture follow underneath. Text is centred horizontally only.
    const bool wordSide = !flipped_;
    const int mainFont = wordSide ? NOTOSERIF_18_FONT_ID : NOTOSERIF_14_FONT_ID;
    const std::string mainText = wordSide ? card_.word : card_.translate;
    const std::string subText = wordSide ? card_.transcription : std::string();

    // Cards carry pre-rendered bitmaps (the host tool converts and resizes
    // them), so showing one is a straight blit: no JPEG/PNG decoder and no
    // extra allocation. The header is read before drawing because the drawn
    // size is part of the block that has to fit the screen.
    std::string imagePath;
    int imageW = 0;
    int imageH = 0;
    bool imageMissing = false;
    if (!card_.image.empty()) {
      imagePath = deck_.folder + "/" + card_.image;
      HalFile probe;
      if (Storage.openFileForRead("ANKI", imagePath, probe)) {
        Bitmap bitmap(probe);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          imageW = bitmap.getWidth();
          imageH = bitmap.getHeight();
        }
        probe.close();
      } else {
        imageMissing = true;
      }
    }

    const auto mainLines = renderer.wrappedText(mainFont, mainText.c_str(), textWidth, CARD_TEXT_LINES);
    std::vector<std::string> subLines;
    if (!subText.empty()) {
      subLines = renderer.wrappedText(NOTOSERIF_14_FONT_ID, subText.c_str(), textWidth, CARD_TEXT_LINES);
    }
    const int mainStep = renderer.getTextHeight(mainFont) + TEXT_LINE_GAP;
    const int subStep = renderer.getTextHeight(NOTOSERIF_14_FONT_ID) + TEXT_LINE_GAP;
    const int textHeight =
        static_cast<int>(mainLines.size()) * mainStep + static_cast<int>(subLines.size()) * subStep;

    // Fit the picture into the space the text leaves over. A deck may ship
    // artwork larger than that space, and drawBitmap scales a bitmap down to
    // the size it is handed — drawing it at its native size would push the
    // picture, or the text, off the panel.
    int imageDrawW = 0;
    int imageDrawH = 0;
    if (imageW > 0 && imageH > 0) {
      const int boxH = (bottom - y) - textHeight - IMAGE_TOP_GAP;
      if (boxH > 0) {
        const float scale = std::min({1.0f, static_cast<float>(textWidth) / static_cast<float>(imageW),
                                      static_cast<float>(boxH) / static_cast<float>(imageH)});
        imageDrawW = std::max(1, static_cast<int>(static_cast<float>(imageW) * scale));
        imageDrawH = std::max(1, static_cast<int>(static_cast<float>(imageH) * scale));
      }
    }

    // The main line (word, or the translation after the flip) is pinned to the
    // top of the content area; the hint and the picture follow it down.
    int lineY = y;

    for (const auto& line : mainLines) {
      renderer.drawCenteredText(mainFont, lineY, line.c_str());
      lineY += mainStep;
    }
    for (const auto& line : subLines) {
      renderer.drawCenteredText(NOTOSERIF_14_FONT_ID, lineY, line.c_str());
      lineY += subStep;
    }

    if (imageDrawW > 0) {
      lineY += IMAGE_TOP_GAP;                                // breathing room under the text
      const int imageX = safe.x + (textWidth - imageDrawW) / 2;  // centre horizontally
      HalFile file;
      if (Storage.openFileForRead("ANKI", imagePath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmap(bitmap, imageX < safe.x ? safe.x : imageX, lineY, imageDrawW, imageDrawH, 0, 0);
        }
        file.close();
      }
    } else if (!card_.image.empty()) {
      LOG_ERR("ANKI", "Card image not usable: %s", imagePath.c_str());
      renderer.drawCenteredText(UI_10_FONT_ID, lineY,
                                imageMissing ? tr(STR_FILE_OPEN_FAILED) : tr(STR_IMAGE_TOO_LARGE));
    }
  }

  const auto labels = missingDeck_
                          ? mappedInput.mapLabels(tr(STR_BACK), "", "", "")
                          : (current_ == SIZE_MAX
                                 ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_REVIEW_ALL), "", "")
                                 : mappedInput.mapLabels(tr(STR_BACK), tr(STR_FLIP), tr(STR_DONT_KNOW),
                                                          tr(STR_KNOW)));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  if (current_ == SIZE_MAX || missingDeck_) {
    // Nothing more to draw: wait for the user to leave (Back) instead of
    // spinning, and make sure the message is on the panel already.
    requestUpdate();
  }
}
