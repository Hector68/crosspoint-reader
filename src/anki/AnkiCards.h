#pragma once
// Anki-style flashcards — see docs/anki-cards.md.
//
// Card *content* is produced off-device: the SD plugin (sd-plugins/anki, or
// scripts/anki_import.py) turns an Anki TSV export into the layout below. The
// firmware never parses .apkg/SQLite and never decodes JPEG/PNG on the review
// path, because ESP32-C3 has ~380 KB RAM and no PSRAM.
//
//   /cards/<topic>/cards.tsv     one card per line, TAB-separated fields:
//                                word \t transcription \t translate [\t image]
//                                or Anki's own export shape, without a
//                                transcription column:
//                                front \t back [\t image]
//                                The last field is the image when it names a
//                                .bmp/.png/.jpg file, which is what tells the
//                                two three-field shapes apart.
//   /cards/<topic>/<image>.bmp   pre-rendered 1bpp images (fourth field)
//   /cards/<topic>/progress.bin  SRS records, written by the device
//   /plugins/anki/cards/…        the same layout, shipped by the SD "anki" plugin
//
// cards.tsv is deliberately Anki's own export shape (fields separated by TABs,
// one record per line) so the plugin only has to strip HTML and resize images.
// Fields must not contain TAB or newline characters; the host tool replaces
// them with spaces. A field may be empty (a word with no transcription is
// written  word<TAB><TAB>translate), but the positions are fixed.
#include <HalStorage.h>
#include <cstdint>
#include <string>
#include <vector>

// One card = one vocabulary entry. The review screen shows word + transcription
// + image first (the prompt), then image + translate after the flip.
struct AnkiCard {
  std::string word;           // big text, e.g. "To be a fashion slave"
  std::string transcription;  // smaller text, e.g. "/tuː biː ə ˈfæʃən sleɪv/"
  std::string translate;      // e.g. "Быть рабом моды"
  std::string image;          // file name inside the deck folder; empty = no image
};

struct AnkiDeckRef {
  std::string title;   // folder name, shown in the UI
  std::string folder;  // SD path, e.g. "/cards/japanese-food"
};

enum class AnkiGrade : uint8_t { Unknown = 0, Again = 1, Good = 2 };

// One SRS record per card, stored as a fixed-size POD at slot `index` of the
// deck's progress.bin. `frontHash` detects a deck the host regenerated with
// different text: the stored schedule is then dropped instead of being applied
// to a different card.
struct AnkiProgressRecord {
  uint32_t frontHash = 0;
  uint32_t dueAtSession = 0;  // deck visit this card becomes due again; 0 = never scheduled
  uint8_t repetitions = 0;    // consecutive "know" answers
  uint8_t againCount = 0;     // "don't know" answers
  uint16_t reserved = 0;      // keeps the record 12 bytes and 4-byte aligned
};
// progress.bin is a fixed-slot file (one slot per card), so the record size is
// part of the on-disk layout and must not change.
static_assert(sizeof(AnkiProgressRecord) == 12, "progress record must stay 12 bytes");

// Scheduling is counted in **deck visits**, not days: the Xteink X4 has no RTC
// and nothing seeds the system clock, so a wall-clock interval would leave every
// card permanently due (the clock reads below 2000-01-01 forever). One session
// is one entry into the deck; the counter lives in progress.bin and survives
// reboots and deep sleep.
namespace AnkiSrs {
// `minSessions` — сколько заходов в колоду карточка, помеченная "I know", не
// показывается. Значение приходит из настройки
// CrossPointSettings::ankiMinSessions, поэтому модуль хранилища не зависит от
// модуля настроек и лишь защитно ограничивает переданное число снизу.
AnkiProgressRecord applyGrade(const AnkiProgressRecord& current, AnkiGrade grade, uint32_t session,
                              uint8_t minSessions);
bool isDue(const AnkiProgressRecord& record, uint32_t session);
}  // namespace AnkiSrs

// One deck = one folder under /cards. Decks are indexed, never loaded: only
// the byte offset of every line is kept in RAM (4 bytes per card), and a
// card's text is read with a single seek+read when it is actually shown.
class AnkiCardStore {
 public:
  // Decks are read from these SD roots, in order. The second one is the folder
  // the SD "anki" plugin ships (see sd-plugins/anki/), so a deck can be added
  // either by running the host tool against the SD card or by dropping the
  // plugin folder onto it.
  static constexpr const char* CARDS_ROOTS[2] = {"/cards", "/plugins/anki/cards"};
  static constexpr uint32_t MAX_CARDS_PER_DECK = 1024;
  static constexpr uint32_t MAX_DECKS = 32;
  static constexpr size_t MAX_FIELD_BYTES = 480;
  static constexpr size_t MAX_IMAGE_NAME_BYTES = 64;
  static constexpr size_t PROGRESS_HEADER_BYTES = 12;  // magic u32 + version u16 + recordSize u16 + session u32

  // FNV-1a/32 of the card's word; the host tool uses the same hash if it
  // pre-seeds progress records.
  static uint32_t hashFront(const std::string& front);

  // Folders under the card roots that contain cards.tsv. An empty result is
  // not an error: it just means no cards have been installed yet.
  static bool scanDecks(std::vector<AnkiDeckRef>& out);

  // Number of records in a deck (streams the file; nothing is kept in RAM).
  static uint32_t countCards(const std::string& folder);

  // Indexes one deck folder: "/cards/<topic>" or "/plugins/anki/cards/<topic>".
  static void scanRoot(const std::string& rootPath, std::vector<AnkiDeckRef>& out);

  // True when at least one deck folder exists (used by the home menu to decide
  // whether to offer "Anki cards" in place of "OPDS Browser").
  static bool hasAnyDeck();

  bool open(const std::string& folder);
  const std::string& folder() const { return folder_; }
  const std::string& title() const { return title_; }
  uint32_t cardCount() const { return static_cast<uint32_t>(offsets_.size()); }
  bool isTruncated() const { return truncated_; }

  bool readCard(size_t index, AnkiCard& out) const;

  // Reads the record stored for `index`. Returns false (and a zeroed record)
  // when the deck has no progress file yet, or when the stored record belongs
  // to different text — callers must then treat the card as new.
  bool loadProgress(size_t index, AnkiProgressRecord& out) const;

  // Starts a deck visit: creates progress.bin when it is missing, replaces it
  // when it carries a different layout, then bumps and persists the session
  // counter. `outSession` receives the number of this visit (1 for the first).
  bool beginSession(uint32_t& outSession);

  // Applies `grade` to the card at `index` and persists the result. `session` is
  // the value beginSession() returned for this visit.
  bool recordGrade(size_t index, AnkiGrade grade, uint32_t session, uint8_t minSessions);

 private:
  std::string cardsPath() const { return folder_ + "/cards.tsv"; }
  std::string progressPath() const { return folder_ + "/progress.bin"; }

  std::string folder_;
  std::string title_;
  std::vector<uint32_t> offsets_;
  bool truncated_ = false;
};

// Indices of the cards to review, in deck order. With dueOnly set, only the
// cards that have come due by `session` are listed; otherwise every card in the
// deck is listed (a manual "review everything" session, which deliberately
// ignores the schedule). Shared by AnkiReviewActivity and the host tests, so the
// rules that decide what the review screen shows are covered off-device.
void buildReviewQueue(const AnkiCardStore& store, uint32_t session, bool dueOnly, std::vector<size_t>& out);
