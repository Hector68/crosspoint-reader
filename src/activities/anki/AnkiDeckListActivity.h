#pragma once
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "../../anki/AnkiCards.h"

/**
 * Lists the Anki decks found on the SD card (/cards/<topic>/cards.tsv).
 * Selecting a deck starts a review session; the last row opens the SD plugin
 * catalog, which is how decks get installed. See docs/anki-cards.md.
 *
 * Row structure (labels/subtitles + the owned strings they point into) is built
 * once in onEnter() — reading card counts touches the SD, so it must not happen
 * per repaint.
 */
class AnkiDeckListActivity final : public UiListActivity {
 public:
  explicit AnkiDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  int listCount() const override { return static_cast<int>(rowItems_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  void rebuildRowItems();

  std::vector<AnkiDeckRef> decks_;
  std::vector<freeink::ui::ListItem> rowItems_;
  // Owned storage for the subtitles rowItems_ points at; never reallocated
  // after the rows are built (reserve() up front), or those pointers dangle.
  std::vector<std::string> subtitles_;
};
