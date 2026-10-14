#pragma once
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "../../anki/AnkiCards.h"

/**
 * Lists the Anki decks found on the SD card (/cards/<topic>/cards.tsv).
 * Selecting a deck starts a review session. Each row shows "Cards: N (M)" — the
 * deck size and how many of those cards the coming visit will actually show.
 * See docs/anki-cards.md.
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
  // Leaving goes home *through the manager* instead of finish(): this screen
  // replaces Home (see ActivityManager::goToAnki), so a plain pop would leave an
  // empty stack and rebuild Home with no menu item selected, parking the cursor
  // on the first row instead of on "Anki cards". goHome() maps the "AnkiDecks"
  // name back to HomeMenuItem::ANKI_CARDS.
  void onBackButton() override { onGoHome(); }

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
