#include "AnkiDeckListActivity.h"

#include <Arduino.h>  // ESP.getFreeHeap for the MEM ledger
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "AnkiReviewActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

AnkiDeckListActivity::AnkiDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("AnkiDecks", renderer, mappedInput) {}

void AnkiDeckListActivity::onEnter() {
  UiListActivity::onEnter();
  nav.selected = 0;
  rebuildRowItems();
  // Heap ledger for field reports: the list keeps only folder names and the
  // per-deck card counts, so it should stay flat next to the review screen.
  LOG_INF("MEM", "anki decks enter: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

const char* AnkiDeckListActivity::headerTitle() const { return tr(STR_ANKI_CARDS); }

// Rows: [deck 0..n-1], [OPDS browser]. 1.6.5 has no plugin catalog, and a deck
// takes the home "library" slot, so the last row is what keeps the OPDS browser
// reachable while decks are installed.
void AnkiDeckListActivity::rebuildRowItems() {
  rowItems_.clear();
  subtitles_.clear();
  decks_.clear();

  if (!AnkiCardStore::scanDecks(decks_)) {
    LOG_DBG("ANKI", "No %s folder on the SD card", AnkiCardStore::CARDS_ROOTS[0]);
    decks_.clear();
  }

  const size_t deckCount = decks_.size();
  rowItems_.reserve(deckCount + 1);
  subtitles_.reserve(deckCount);

  for (size_t i = 0; i < deckCount; ++i) {
    const uint32_t cards = AnkiCardStore::countCards(decks_[i].folder);
    char count[48];
    snprintf(count, sizeof(count), tr(STR_N_CARDS), static_cast<unsigned>(cards));
    subtitles_.push_back(count);

    fui::ListItem item;
    item.label = decks_[i].title.c_str();
    item.subtitle = subtitles_.back().c_str();
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }

  fui::ListItem opdsRow;
  opdsRow.label = tr(STR_OPDS_BROWSER);
  opdsRow.actionValue = static_cast<int16_t>(deckCount);
  rowItems_.push_back(opdsRow);
}

void AnkiDeckListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the header band, above the button hints; derived from the
  // safe area so board bezel insets apply (same as OpdsServerListActivity).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (decks_.empty()) {
    screen.centeredText(tr(STR_NO_CARDS_FOUND), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}

void AnkiDeckListActivity::activateIndex(const int index) {
  nav.selected = index;
  // Activation opens another screen; a lingering flash would gray an unrelated
  // row on the next render.
  app.clearTapFlash();

  if (index < 0 || index >= static_cast<int>(decks_.size())) {
    // Last row: the OPDS browser (1.6.5 has no plugin catalog).
    activityManager.goToBrowser();
    return;
  }

  auto activity = makeUniqueNoThrow<AnkiReviewActivity>(renderer, mappedInput, decks_[index]);
  if (!activity) {
    LOG_ERR("ANKI", "OOM: anki review activity");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    // The deck contents may have changed while the review screen was open
    // (e.g. the user re-imported through the plugin's web page).
    nav.selected = 0;
    rebuildRowItems();
  });
}
