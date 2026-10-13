#pragma once
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "../../anki/AnkiCards.h"

/**
 * One Anki review session over a deck folder (/cards/<topic>).
 *
 * Buttons (standard 4-key layout, so every action also follows the user's
 * custom front-button remapping): Back = leave, Confirm = flip the card,
 * Left = "I don't know", Right = "I know".
 *
 * Cards whose spaced-repetition interval has not elapsed are skipped, and a
 * card answered "I don't know" is put back at the end of the session so it
 * comes round again. See docs/anki-cards.md.
 *
 * Plain (non-FreeInkUI) activity: it draws text and blits pre-rendered 1bpp
 * BMPs directly through GfxRenderer, so no image decoding or float arithmetic
 * happens on the device.
 */
class AnkiReviewActivity final : public Activity {
 public:
  AnkiReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, AnkiDeckRef deck);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void draw();
  void buildQueue(bool dueOnly);
  void answer(AnkiGrade grade);
  void showCard(size_t index);

  AnkiDeckRef deck_;
  AnkiCardStore store_;
  AnkiCard card_;
  // Card indices still to show in this session (built on entry, extended with
  // cards answered "I don't know").
  std::vector<size_t> queue_;
  size_t queuePos_ = 0;
  size_t current_ = SIZE_MAX;  // SIZE_MAX = session finished / nothing to review
  bool flipped_ = false;
  uint32_t knownCount_ = 0;
  bool missingDeck_ = false;
  // False once the user chose "Review all": the queue then holds every card,
  // not only the ones whose interval has elapsed.
  bool dueOnly_ = true;
  // Distinguishes "Review complete" (a session ran) from "Nothing due"
  // (every card is still scheduled in the future).
  bool reviewedAnything_ = false;
  uint32_t nowEpoch_ = 0;
};
