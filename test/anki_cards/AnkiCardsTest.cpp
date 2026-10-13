// Host tests for the Anki card store and its SM-2-lite schedule.
// The storage layer is the project's fake HalStorage (test/library_builder/stubs),
// which keeps the whole SD card in a std::map, so these tests need no hardware.

#include <gtest/gtest.h>

#include <anki/AnkiCards.h>
#include <HalStorage.h>

namespace {
// 2030-05-13: comfortably after AnkiCardStore::MIN_VALID_EPOCH (2000-01-01),
// so intervals are actually counted instead of being ignored.
constexpr uint32_t kNow = 1900000000u;
constexpr uint32_t kDay = 86400u;

std::string cardsTsv(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& line : lines) out += line + "\n";
  return out;
}
}  // namespace

// ---------------------------------------------------------------------------
// AnkiSrs: the schedule is pure integer math, so it is fully covered here.
// ---------------------------------------------------------------------------

TEST(AnkiSrs, NewCardIsAlwaysDue) {
  EXPECT_TRUE(AnkiSrs::isDue(AnkiProgressRecord{}, kNow));
}

TEST(AnkiSrs, AgainAnswerIsAlwaysDue) {
  // An "I don't know" answer resets the interval to 0, so the card is due
  // again no matter how much time has passed.
  AnkiProgressRecord record{};
  record.repetitions = 6;
  record.intervalDays = 120;
  record.lastReviewed = kNow;
  record = AnkiSrs::applyGrade(record, AnkiGrade::Again, kNow);
  EXPECT_EQ(record.intervalDays, 0u);
  EXPECT_TRUE(AnkiSrs::isDue(record, kNow + 30 * kDay));
}

TEST(AnkiSrs, GoodMovesUpTheIntervalLadder) {
  AnkiProgressRecord record{};
  record = AnkiSrs::applyGrade(record, AnkiGrade::Good, kNow);
  EXPECT_EQ(record.repetitions, 1u);
  EXPECT_EQ(record.intervalDays, 1u);  // ladder[1]
  EXPECT_EQ(record.lastReviewed, kNow);

  record = AnkiSrs::applyGrade(record, AnkiGrade::Good, kNow);
  EXPECT_EQ(record.repetitions, 2u);
  EXPECT_EQ(record.intervalDays, 3u);  // ladder[2]

  record = AnkiSrs::applyGrade(record, AnkiGrade::Good, kNow);
  EXPECT_EQ(record.intervalDays, 7u);  // ladder[3]
}

TEST(AnkiSrs, IntervalLadderCapsAtItsLastStep) {
  AnkiProgressRecord record{};
  for (int i = 0; i < 12; ++i) record = AnkiSrs::applyGrade(record, AnkiGrade::Good, kNow);
  EXPECT_EQ(record.repetitions, AnkiSrs::INTERVAL_COUNT);
  EXPECT_EQ(record.intervalDays, AnkiSrs::INTERVAL_DAYS[AnkiSrs::INTERVAL_COUNT - 1]);
}

TEST(AnkiSrs, AgainResetsTheScheduleAndCounts) {
  AnkiProgressRecord record{};
  record.repetitions = 3;
  record.intervalDays = 7;
  record.againCount = 1;
  record = AnkiSrs::applyGrade(record, AnkiGrade::Again, kNow);
  EXPECT_EQ(record.repetitions, 0u);
  EXPECT_EQ(record.intervalDays, 0u);  // due again immediately
  EXPECT_EQ(record.againCount, 2u);
  EXPECT_EQ(record.lastReviewed, kNow);
}

TEST(AnkiSrs, CardBecomesDueOnceItsIntervalHasPassed) {
  AnkiProgressRecord record{};
  record.repetitions = 1;
  record.intervalDays = 1;
  record.lastReviewed = kNow;
  EXPECT_FALSE(AnkiSrs::isDue(record, kNow));
  EXPECT_FALSE(AnkiSrs::isDue(record, kNow + kDay - 1));
  EXPECT_TRUE(AnkiSrs::isDue(record, kNow + kDay));
}

TEST(AnkiSrs, UnsyncedClockKeepsEveryCardDue) {
  // Before 2000-01-01 the host has not set the clock (no NTP yet); intervals
  // would be meaningless, so nothing is allowed to hide behind one.
  AnkiProgressRecord record{};
  record.repetitions = 7;
  record.intervalDays = 120;
  record.lastReviewed = kNow;
  EXPECT_TRUE(AnkiSrs::isDue(record, AnkiCardStore::MIN_VALID_EPOCH - 1));
}

TEST(AnkiSrs, UnknownGradeOnlyStampsTheReviewTime) {
  AnkiProgressRecord record{};
  record.repetitions = 2;
  record.intervalDays = 3;
  record = AnkiSrs::applyGrade(record, AnkiGrade::Unknown, kNow);
  EXPECT_EQ(record.lastReviewed, kNow);
  EXPECT_EQ(record.repetitions, 2u);
  EXPECT_EQ(record.intervalDays, 3u);
}

// ---------------------------------------------------------------------------
// AnkiCardStore: indexing, reading and progress persistence.
// ---------------------------------------------------------------------------

TEST(AnkiCardStore, HashFrontIsStableAndTextSensitive) {
  EXPECT_EQ(AnkiCardStore::hashFront("cat"), AnkiCardStore::hashFront("cat"));
  EXPECT_NE(AnkiCardStore::hashFront("cat"), AnkiCardStore::hashFront("dog"));
  EXPECT_EQ(sizeof(AnkiProgressRecord), 12u);  // layout is shared with the host tool
}

TEST(AnkiCardStore, ScanDecksFindsDecksUnderBothRoots) {
  fake::reset();
  fake::add("/cards/english/cards.tsv", "cat\tneko\n");
  fake::add("/plugins/anki/cards/spanish/cards.tsv", "gato\tgato\n");
  std::vector<AnkiDeckRef> decks;
  ASSERT_TRUE(AnkiCardStore::scanDecks(decks));
  ASSERT_EQ(decks.size(), 2u);
  EXPECT_EQ(decks[0].folder, "/cards/english");
  EXPECT_EQ(decks[0].title, "english");
  EXPECT_EQ(decks[1].folder, "/plugins/anki/cards/spanish");
  EXPECT_EQ(decks[1].title, "spanish");
}

TEST(AnkiCardStore, ScanDecksSkipsFoldersWithoutADeck) {
  fake::reset();
  fake::add("/cards/empty/notes.txt", "not a deck");
  std::vector<AnkiDeckRef> decks;
  EXPECT_FALSE(AnkiCardStore::scanDecks(decks));
  EXPECT_TRUE(decks.empty());
}

TEST(AnkiCardStore, HasAnyDeckReflectsTheCardRoots) {
  fake::reset();
  EXPECT_FALSE(AnkiCardStore::hasAnyDeck());
  fake::add("/plugins/anki/cards/spanish/cards.tsv", "gato\tgato\n");
  EXPECT_TRUE(AnkiCardStore::hasAnyDeck());
  fake::reset();
  fake::add("/cards/english/cards.tsv", "cat\tneko\n");
  EXPECT_TRUE(AnkiCardStore::hasAnyDeck());
}

TEST(AnkiCardStore, OpenIndexesEveryRecord) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv",
            cardsTsv({"cat\tkat\tneko\timg_001.bmp", "dog\tdogo\tinu\timg_002.bmp", "bird\tbado\tsuzume\timg_003.bmp"}));
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  EXPECT_EQ(store.title(), "demo");
  EXPECT_EQ(store.cardCount(), 3u);
  EXPECT_FALSE(store.isTruncated());

  AnkiCard card;
  ASSERT_TRUE(store.readCard(1, card));
  EXPECT_EQ(card.word, "dog");
  EXPECT_EQ(card.transcription, "dogo");
  EXPECT_EQ(card.translate, "inu");
  EXPECT_EQ(card.image, "img_002.bmp");
}

TEST(AnkiCardStore, OpenHandlesFinalRecordWithoutNewline) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "one\ttwo\nthree\tfour");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  EXPECT_EQ(store.cardCount(), 2u);
}

TEST(AnkiCardStore, OpenFailsWhenTheDeckFileIsMissing) {
  fake::reset();
  AnkiCardStore store;
  EXPECT_FALSE(store.open("/cards/absent"));
}

TEST(AnkiCardStore, OpenStopsAtTheCardLimit) {
  fake::reset();
  std::vector<std::string> lines;
  lines.reserve(AnkiCardStore::MAX_CARDS_PER_DECK + 5);
  for (uint32_t i = 0; i < AnkiCardStore::MAX_CARDS_PER_DECK + 5; ++i) lines.push_back("word\ttrans\ttranslate");
  fake::add("/cards/huge/cards.tsv", cardsTsv(lines));
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/huge"));
  EXPECT_EQ(store.cardCount(), AnkiCardStore::MAX_CARDS_PER_DECK);
  EXPECT_TRUE(store.isTruncated());
}

TEST(AnkiCardStore, ReadCardSplitsWordTranscriptionTranslateImage) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "To be a fashion slave\ttu bi a fashion sleiv\tByt' rabom mody\timg_001.bmp\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  AnkiCard card;
  ASSERT_TRUE(store.readCard(0, card));
  EXPECT_EQ(card.word, "To be a fashion slave");
  EXPECT_EQ(card.transcription, "tu bi a fashion sleiv");
  EXPECT_EQ(card.translate, "Byt' rabom mody");
  EXPECT_EQ(card.image, "img_001.bmp");
}

TEST(AnkiCardStore, ReadCardAllowsAnEmptyTranscriptionField) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "hello\t\tprivet\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  AnkiCard card;
  ASSERT_TRUE(store.readCard(0, card));
  EXPECT_EQ(card.word, "hello");
  EXPECT_TRUE(card.transcription.empty());
  EXPECT_EQ(card.translate, "privet");
}

TEST(AnkiCardStore, ReadCardTreatsTwoFieldsAsWordAndTranslate) {
  // Anki's plain "Front<TAB>Back" export: there is no transcription column, so
  // the second field is the answer, not a pronunciation hint.
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  AnkiCard card;
  ASSERT_TRUE(store.readCard(0, card));
  EXPECT_EQ(card.word, "cat");
  EXPECT_TRUE(card.transcription.empty());
  EXPECT_EQ(card.translate, "neko");
  EXPECT_TRUE(card.image.empty());
}

TEST(AnkiCardStore, ReadCardAcceptsAnkiFrontBackImageRecords) {
  // scripts/anki_import.py (and docs/anki-cards.md) write three fields:
  // front, back, image. The image name must land in `image`, not in
  // `translate`, or the review screen shows the file name instead of the
  // picture and the answer leaks onto the front of the card.
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "What is the capital of Japan?\tTokyo\timg_001.bmp\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  AnkiCard card;
  ASSERT_TRUE(store.readCard(0, card));
  EXPECT_EQ(card.word, "What is the capital of Japan?");
  EXPECT_TRUE(card.transcription.empty());
  EXPECT_EQ(card.translate, "Tokyo");
  EXPECT_EQ(card.image, "img_001.bmp");
}

TEST(AnkiCardStore, ReadCardTreatsThreeTextFieldsAsWordTranscriptionTranslate) {
  // Three fields where the last one is not a picture name keep the canonical
  // meaning, so a deck without images still carries its pronunciation hint.
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "apple\tAP-ul\tyabloko\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  AnkiCard card;
  ASSERT_TRUE(store.readCard(0, card));
  EXPECT_EQ(card.word, "apple");
  EXPECT_EQ(card.transcription, "AP-ul");
  EXPECT_EQ(card.translate, "yabloko");
  EXPECT_TRUE(card.image.empty());
}

TEST(AnkiCardStore, ReadCardRejectsRecordsWithoutATab) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "this is not a card\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  AnkiCard card;
  EXPECT_FALSE(store.readCard(0, card));
}

TEST(AnkiCardStore, ReadCardRejectsUnknownIndex) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  AnkiCard card;
  EXPECT_FALSE(store.readCard(1, card));
}

TEST(AnkiCardStore, CountCardsCountsRecords) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", cardsTsv({"a\tb", "c\td"}));
  EXPECT_EQ(AnkiCardStore::countCards("/cards/demo"), 2u);
}

TEST(AnkiCardStore, RecordGradeWritesASrsRecord) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  EXPECT_TRUE(store.recordGrade(0, AnkiGrade::Good, kNow));

  // header (8 bytes) + one record (12 bytes)
  ASSERT_TRUE(fake::files.count("/cards/demo/progress.bin"));
  EXPECT_EQ(fake::files["/cards/demo/progress.bin"]->bytes.size(),
            AnkiCardStore::PROGRESS_HEADER_BYTES + sizeof(AnkiProgressRecord));

  AnkiProgressRecord record;
  ASSERT_TRUE(store.loadProgress(0, record));
  EXPECT_EQ(record.frontHash, AnkiCardStore::hashFront("cat"));
  EXPECT_EQ(record.repetitions, 1u);
  EXPECT_EQ(record.intervalDays, 1u);
  EXPECT_EQ(record.againCount, 0u);
  EXPECT_EQ(record.lastReviewed, kNow);
}

TEST(AnkiCardStore, RecordGradeDropsProgressFromDifferentText) {
  // The host regenerated the deck: the old record's frontHash no longer
  // matches, so it must be discarded instead of applied to the new card.
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "dog\tinu\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  ASSERT_TRUE(store.recordGrade(0, AnkiGrade::Good, kNow));

  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  ASSERT_TRUE(store.open("/cards/demo"));
  EXPECT_TRUE(store.recordGrade(0, AnkiGrade::Good, kNow + kDay));

  AnkiProgressRecord record;
  ASSERT_TRUE(store.loadProgress(0, record));
  EXPECT_EQ(record.frontHash, AnkiCardStore::hashFront("cat"));
  EXPECT_EQ(record.repetitions, 1u);
  EXPECT_EQ(record.intervalDays, 1u);
  EXPECT_EQ(record.lastReviewed, kNow + kDay);
}

TEST(AnkiCardStore, RecordGradeAgainResetsTheStoredSchedule) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  ASSERT_TRUE(store.recordGrade(0, AnkiGrade::Good, kNow));
  EXPECT_TRUE(store.recordGrade(0, AnkiGrade::Again, kNow + kDay));

  AnkiProgressRecord record;
  ASSERT_TRUE(store.loadProgress(0, record));
  EXPECT_EQ(record.repetitions, 0u);
  EXPECT_EQ(record.intervalDays, 0u);
  EXPECT_EQ(record.againCount, 1u);
  EXPECT_EQ(record.lastReviewed, kNow + kDay);
}

TEST(AnkiCardStore, LoadProgressRejectsACorruptFile) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  fake::add("/cards/demo/progress.bin", "not a progress file");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  AnkiProgressRecord record{};
  record.repetitions = 9;
  EXPECT_FALSE(store.loadProgress(0, record));
  EXPECT_EQ(record.repetitions, 0u);  // zeroed, not left half-read
}

TEST(AnkiCardStore, RecordGradeFailsOnAnEmptyDeck) {
  fake::reset();
  AnkiCardStore store;
  EXPECT_FALSE(store.recordGrade(0, AnkiGrade::Good, kNow));
}

// ---------------------------------------------------------------------------
// The review screen's queue: which cards the user can actually reach.
// ---------------------------------------------------------------------------

TEST(AnkiReviewQueue, DueOnlyQueueShrinksAsCardsAreScheduled) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", cardsTsv({"one\ttwo", "three\tfour", "five\tsix"}));
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));

  std::vector<size_t> queue;
  buildReviewQueue(store, kNow, true, queue);
  ASSERT_EQ(queue.size(), 3u);  // nothing reviewed yet -> every card is due

  // Answering a card "I know" starts its interval, so a *due only* build must
  // not offer it again.
  ASSERT_TRUE(store.recordGrade(1, AnkiGrade::Good, kNow));
  buildReviewQueue(store, kNow, true, queue);
  ASSERT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue[0], 0u);
  EXPECT_EQ(queue[1], 2u);

  // "Review all" (Confirm on the Review complete screen) lists every card,
  // which is how the user gets back into a deck whose cards are scheduled.
  buildReviewQueue(store, kNow, false, queue);
  ASSERT_EQ(queue.size(), 3u);
  EXPECT_EQ(queue[0], 0u);
  EXPECT_EQ(queue[1], 1u);
  EXPECT_EQ(queue[2], 2u);
}

TEST(AnkiReviewQueue, DueOnlyQueueIsEmptyWhenEverythingIsScheduled) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", cardsTsv({"one\ttwo", "three\tfour"}));
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  ASSERT_TRUE(store.recordGrade(0, AnkiGrade::Good, kNow));
  ASSERT_TRUE(store.recordGrade(1, AnkiGrade::Good, kNow));

  std::vector<size_t> queue;
  buildReviewQueue(store, kNow, true, queue);
  EXPECT_TRUE(queue.empty());  // -> the review screen shows "Nothing due"

  buildReviewQueue(store, kNow, false, queue);
  EXPECT_EQ(queue.size(), 2u);  // -> Confirm starts a fresh full-deck session
}
