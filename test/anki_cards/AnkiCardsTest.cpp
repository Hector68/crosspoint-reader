// Host tests for the Anki card store and its visit-based schedule.
// The storage layer is the project's fake HalStorage (test/library_builder/stubs),
// which keeps the whole SD card in a std::map, so these tests need no hardware.

#include <gtest/gtest.h>

#include <anki/AnkiCards.h>
#include <HalStorage.h>

namespace {
// The schedule is counted in deck visits, not days: the Xteink X4 has no RTC and
// nothing seeds the system clock, so a wall-clock interval would leave every card
// permanently due. A "session" is just a small integer here.
constexpr uint32_t kSession = 10;
constexpr uint8_t kGap = 4;  // CrossPointSettings::DEFAULT_ANKI_MIN_SESSIONS

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
  EXPECT_TRUE(AnkiSrs::isDue(AnkiProgressRecord{}, kSession));
}

TEST(AnkiSrs, GoodParksTheCardForTheConfiguredGap) {
  AnkiProgressRecord record{};
  record = AnkiSrs::applyGrade(record, AnkiGrade::Good, kSession, kGap);
  EXPECT_EQ(record.repetitions, 1u);
  EXPECT_EQ(record.dueAtSession, kSession + kGap);
  // Hidden on the visits in between, due exactly on the gap-th one.
  EXPECT_FALSE(AnkiSrs::isDue(record, kSession));
  EXPECT_FALSE(AnkiSrs::isDue(record, kSession + kGap - 1));
  EXPECT_TRUE(AnkiSrs::isDue(record, kSession + kGap));
}

TEST(AnkiSrs, GoodKeepsAFixedGapHoweverOftenItIsAnswered) {
  // A growing ladder would hide a well-known card for months once the unit is a
  // deck visit, so the gap stays put and only the counters grow.
  AnkiProgressRecord record{};
  for (int i = 0; i < 12; ++i) {
    record = AnkiSrs::applyGrade(record, AnkiGrade::Good, kSession, kGap);
    EXPECT_EQ(record.dueAtSession, kSession + kGap);
  }
  EXPECT_EQ(record.repetitions, 12u);
}

TEST(AnkiSrs, AgainAnswerIsDueImmediately) {
  AnkiProgressRecord record{};
  record.repetitions = 6;
  record.dueAtSession = kSession + 120;
  record = AnkiSrs::applyGrade(record, AnkiGrade::Again, kSession, kGap);
  EXPECT_EQ(record.repetitions, 0u);
  EXPECT_EQ(record.dueAtSession, kSession);
  EXPECT_TRUE(AnkiSrs::isDue(record, kSession));
  // ...and it stays due however many visits pass, so a failed card cannot hide.
  EXPECT_TRUE(AnkiSrs::isDue(record, kSession + 30));
}

TEST(AnkiSrs, AgainCountsAndResetsRepetitions) {
  AnkiProgressRecord record{};
  record.repetitions = 3;
  record.dueAtSession = kSession + 7;
  record.againCount = 1;
  record = AnkiSrs::applyGrade(record, AnkiGrade::Again, kSession, kGap);
  EXPECT_EQ(record.repetitions, 0u);
  EXPECT_EQ(record.againCount, 2u);
}

TEST(AnkiSrs, ZeroGapCannotMakeAKnownCardDueImmediately) {
  // The Settings row enforces 1..30, but a corrupt settings byte must not turn
  // "I know" into a no-op.
  AnkiProgressRecord record{};
  record = AnkiSrs::applyGrade(record, AnkiGrade::Good, kSession, 0);
  EXPECT_EQ(record.dueAtSession, kSession + 1);
  EXPECT_FALSE(AnkiSrs::isDue(record, kSession));
}

TEST(AnkiSrs, UnknownGradeChangesNothing) {
  AnkiProgressRecord record{};
  record.repetitions = 2;
  record.dueAtSession = kSession + 3;
  record = AnkiSrs::applyGrade(record, AnkiGrade::Unknown, kSession, kGap);
  EXPECT_EQ(record.repetitions, 2u);
  EXPECT_EQ(record.dueAtSession, kSession + 3);
}

// ---------------------------------------------------------------------------
// AnkiCardStore: indexing, reading and progress persistence.
// ---------------------------------------------------------------------------

TEST(AnkiCardStore, HashFrontIsStableAndTextSensitive) {
  EXPECT_EQ(AnkiCardStore::hashFront("cat"), AnkiCardStore::hashFront("cat"));
  EXPECT_NE(AnkiCardStore::hashFront("cat"), AnkiCardStore::hashFront("dog"));
  EXPECT_EQ(sizeof(AnkiProgressRecord), 12u);  // progress.bin is a fixed-slot file
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

TEST(AnkiCardStore, ReadCardReadsFourTextFieldsWithContext) {
  // word, transcription, translate, context -- no picture. The 4th field is not
  // an image name, so it is text, not the image.
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "apple\tAP-ul\tyabloko\tShe ate an apple for breakfast.\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  AnkiCard card;
  ASSERT_TRUE(store.readCard(0, card));
  EXPECT_EQ(card.word, "apple");
  EXPECT_EQ(card.transcription, "AP-ul");
  EXPECT_EQ(card.translate, "yabloko");
  EXPECT_EQ(card.context, "She ate an apple for breakfast.");
  EXPECT_TRUE(card.image.empty());
}

TEST(AnkiCardStore, ReadCardKeepsTheImageLastWhenContextIsPresent) {
  // The full five positions: the picture stays last, so the field before it is
  // context rather than an image name.
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "apple\tAP-ul\tyabloko\tShe ate an apple for breakfast.\tapple.bmp\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  AnkiCard card;
  ASSERT_TRUE(store.readCard(0, card));
  EXPECT_EQ(card.word, "apple");
  EXPECT_EQ(card.transcription, "AP-ul");
  EXPECT_EQ(card.translate, "yabloko");
  EXPECT_EQ(card.context, "She ate an apple for breakfast.");
  EXPECT_EQ(card.image, "apple.bmp");
}

TEST(AnkiCardStore, ReadCardTreatsATrailingEmptyFieldAsAnEmptyContext) {
  // An empty trailing field is not an image name, so a four-field record with a
  // dangling tab reads as an (empty) context rather than as a picture.
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "apple\tAP-ul\tyabloko\t\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  AnkiCard card;
  ASSERT_TRUE(store.readCard(0, card));
  EXPECT_EQ(card.translate, "yabloko");
  EXPECT_TRUE(card.context.empty());
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

TEST(AnkiCardStore, RecordGradeWritesASchedule) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  EXPECT_TRUE(store.recordGrade(0, AnkiGrade::Good, kSession, kGap));

  // header (magic + version + record size + session counter) + one record
  ASSERT_TRUE(fake::files.count("/cards/demo/progress.bin"));
  EXPECT_EQ(fake::files["/cards/demo/progress.bin"]->bytes.size(),
            AnkiCardStore::PROGRESS_HEADER_BYTES + sizeof(AnkiProgressRecord));

  AnkiProgressRecord record;
  ASSERT_TRUE(store.loadProgress(0, record));
  EXPECT_EQ(record.frontHash, AnkiCardStore::hashFront("cat"));
  EXPECT_EQ(record.repetitions, 1u);
  EXPECT_EQ(record.dueAtSession, kSession + kGap);
  EXPECT_EQ(record.againCount, 0u);
}

TEST(AnkiCardStore, RecordGradeDropsProgressFromDifferentText) {
  // The host regenerated the deck: the old record's frontHash no longer
  // matches, so it must be discarded instead of applied to the new card.
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "dog\tinu\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  ASSERT_TRUE(store.recordGrade(0, AnkiGrade::Good, kSession, kGap));

  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  ASSERT_TRUE(store.open("/cards/demo"));
  EXPECT_TRUE(store.recordGrade(0, AnkiGrade::Good, kSession + 1, kGap));

  AnkiProgressRecord record;
  ASSERT_TRUE(store.loadProgress(0, record));
  EXPECT_EQ(record.frontHash, AnkiCardStore::hashFront("cat"));
  EXPECT_EQ(record.repetitions, 1u);  // not 2: the stale schedule was dropped
  EXPECT_EQ(record.dueAtSession, kSession + 1 + kGap);
}

TEST(AnkiCardStore, RecordGradeAgainResetsTheStoredSchedule) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  ASSERT_TRUE(store.recordGrade(0, AnkiGrade::Good, kSession, kGap));
  EXPECT_TRUE(store.recordGrade(0, AnkiGrade::Again, kSession + 1, kGap));

  AnkiProgressRecord record;
  ASSERT_TRUE(store.loadProgress(0, record));
  EXPECT_EQ(record.repetitions, 0u);
  EXPECT_EQ(record.dueAtSession, kSession + 1);  // due again immediately
  EXPECT_EQ(record.againCount, 1u);
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
  EXPECT_FALSE(store.recordGrade(0, AnkiGrade::Good, kSession, kGap));
}

TEST(AnkiCardStore, BeginSessionCreatesAndIncrementsTheCounter) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));

  uint32_t session = 0;
  ASSERT_TRUE(store.beginSession(session));
  EXPECT_EQ(session, 1u);
  ASSERT_TRUE(store.beginSession(session));
  EXPECT_EQ(session, 2u);

  // The counter lives on the card, so a fresh store (the next visit, after a
  // reboot or a deep-sleep wake) continues the sequence instead of restarting.
  AnkiCardStore reopened;
  ASSERT_TRUE(reopened.open("/cards/demo"));
  ASSERT_TRUE(reopened.beginSession(session));
  EXPECT_EQ(session, 3u);
}

TEST(AnkiCardStore, BeginSessionReplacesAFileFromAnOlderLayout) {
  // v1 stored day intervals and had a shorter header. The record offsets depend
  // on the header size, so a stale file must be replaced, never appended to.
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  const std::string v1Header = std::string("CPAK\x01\x00\x0c\x00", 8) + std::string(12, '\0');
  fake::add("/cards/demo/progress.bin", v1Header);
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));

  uint32_t session = 0;
  ASSERT_TRUE(store.beginSession(session));
  EXPECT_EQ(session, 1u);

  // The rewritten file is readable and carries an empty slot for the card.
  AnkiProgressRecord record;
  ASSERT_TRUE(store.loadProgress(0, record));
  EXPECT_EQ(record.frontHash, 0u);
  EXPECT_EQ(record.dueAtSession, 0u);
  EXPECT_TRUE(AnkiSrs::isDue(record, session));
}

TEST(AnkiCardStore, BeginSessionFailsOnAnEmptyDeck) {
  fake::reset();
  AnkiCardStore store;
  uint32_t session = 99;
  EXPECT_FALSE(store.beginSession(session));
  EXPECT_EQ(session, 0u);
}

// ---------------------------------------------------------------------------
// The review screen's queue: which cards the user can actually reach.
// ---------------------------------------------------------------------------

TEST(AnkiReviewQueue, KnownCardsLeaveTheDueQueueUntilTheGapElapses) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", cardsTsv({"one\ttwo", "three\tfour", "five\tsix"}));
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));

  std::vector<size_t> queue;
  buildReviewQueue(store, kSession, true, queue);
  ASSERT_EQ(queue.size(), 3u);  // nothing reviewed yet -> every card is due

  // "I know" parks the card for kGap visits, so a *due only* build must not
  // offer it again until then.
  ASSERT_TRUE(store.recordGrade(1, AnkiGrade::Good, kSession, kGap));
  buildReviewQueue(store, kSession, true, queue);
  ASSERT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue[0], 0u);
  EXPECT_EQ(queue[1], 2u);

  buildReviewQueue(store, kSession + kGap - 1, true, queue);
  EXPECT_EQ(queue.size(), 2u);  // still parked on the visit before the gap
  buildReviewQueue(store, kSession + kGap, true, queue);
  EXPECT_EQ(queue.size(), 3u);  // back on the gap-th visit

  // "Review all" (Confirm on the Review complete screen) deliberately ignores
  // the schedule, which is how the user drills a deck out of turn.
  buildReviewQueue(store, kSession, false, queue);
  ASSERT_EQ(queue.size(), 3u);
  EXPECT_EQ(queue[0], 0u);
  EXPECT_EQ(queue[1], 1u);
  EXPECT_EQ(queue[2], 2u);
}

TEST(AnkiReviewQueue, DueOnlyQueueIsEmptyWhenEverythingIsParked) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", cardsTsv({"one\ttwo", "three\tfour"}));
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  ASSERT_TRUE(store.recordGrade(0, AnkiGrade::Good, kSession, kGap));
  ASSERT_TRUE(store.recordGrade(1, AnkiGrade::Good, kSession, kGap));

  std::vector<size_t> queue;
  buildReviewQueue(store, kSession, true, queue);
  EXPECT_TRUE(queue.empty());  // -> the review screen shows "Nothing due"

  buildReviewQueue(store, kSession, false, queue);
  EXPECT_EQ(queue.size(), 2u);  // -> Confirm starts a fresh full-deck session
}

// ---------------------------------------------------------------------------
// The deck list preview: "Cards: N, Active: M".
// ---------------------------------------------------------------------------

TEST(AnkiDeckPreview, StoredSessionReportsTheCounterWithoutBumpingIt) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  EXPECT_EQ(AnkiCardStore::storedSession("/cards/demo"), 0u);  // never reviewed

  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  uint32_t session = 0;
  ASSERT_TRUE(store.beginSession(session));
  ASSERT_TRUE(store.beginSession(session));
  EXPECT_EQ(session, 2u);
  EXPECT_EQ(AnkiCardStore::storedSession("/cards/demo"), 2u);

  // Reading it must not advance the deck: the preview runs on the list screen,
  // long before the user picks this deck, and a peek that counted as a visit
  // would hide a card one visit longer than the gap promises.
  EXPECT_EQ(AnkiCardStore::storedSession("/cards/demo"), 2u);
  ASSERT_TRUE(store.beginSession(session));
  EXPECT_EQ(session, 3u);
}

TEST(AnkiDeckPreview, StoredSessionIsZeroForAFileFromAnOlderLayout) {
  // beginSession() replaces such a file and starts over at zero, so the preview
  // has to count from zero as well.
  fake::reset();
  fake::add("/cards/demo/cards.tsv", "cat\tneko\n");
  fake::add("/cards/demo/progress.bin", std::string("CPAK\x01\x00\x0c\x00", 8) + std::string(12, '\0'));
  EXPECT_EQ(AnkiCardStore::storedSession("/cards/demo"), 0u);
}

TEST(AnkiDeckPreview, CountDueCardsTreatsADeckWithoutProgressAsAllNew) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", cardsTsv({"one\ttwo", "three\tfour", "five\tsix", "seven\teight"}));
  EXPECT_EQ(AnkiCardStore::countDueCards("/cards/demo", kSession, 4u), 4u);
  EXPECT_EQ(AnkiCardStore::countDueCards("/cards/absent", kSession, 0u), 0u);
}

TEST(AnkiDeckPreview, CountDueCardsHidesAParkedCardUntilItsGapElapses) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", cardsTsv({"one\ttwo", "three\tfour", "five\tsix", "seven\teight"}));
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  ASSERT_TRUE(store.recordGrade(1, AnkiGrade::Good, kSession, kGap));

  EXPECT_EQ(AnkiCardStore::countDueCards("/cards/demo", kSession, 4u), 3u);  // parked
  EXPECT_EQ(AnkiCardStore::countDueCards("/cards/demo", kSession + kGap, 4u), 4u);  // back
}

TEST(AnkiDeckPreview, CountDueCardsTreatsSlotsPastATruncatedFileAsNew) {
  fake::reset();
  fake::add("/cards/demo/cards.tsv", cardsTsv({"one\ttwo", "three\tfour", "five\tsix"}));
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));
  ASSERT_TRUE(store.recordGrade(0, AnkiGrade::Good, kSession, kGap));

  // Cut the file after the header and the first slot: the missing records carry
  // no schedule, which is what a card that was never answered looks like.
  fake::files["/cards/demo/progress.bin"]->bytes.resize(AnkiCardStore::PROGRESS_HEADER_BYTES +
                                                         sizeof(AnkiProgressRecord));
  EXPECT_EQ(AnkiCardStore::countDueCards("/cards/demo", kSession, 3u), 2u);
}

TEST(AnkiDeckPreview, CountDueCardsMatchesTheReviewQueue) {
  // "Active" has to be the number the review screen really shows, for the visit
  // beginSession() is about to hand out -- the deck list previews at
  // storedSession() + 1. This is the test that keeps the two from drifting.
  fake::reset();
  fake::add("/cards/demo/cards.tsv", cardsTsv({"one\ttwo", "three\tfour", "five\tsix"}));
  AnkiCardStore store;
  ASSERT_TRUE(store.open("/cards/demo"));

  uint32_t session = 0;
  ASSERT_TRUE(store.beginSession(session));
  ASSERT_TRUE(store.recordGrade(0, AnkiGrade::Good, session, kGap));

  const auto preview = [&store]() {
    return AnkiCardStore::countDueCards("/cards/demo", AnkiCardStore::storedSession("/cards/demo") + 1, 3u);
  };
  uint32_t previewed = preview();
  EXPECT_EQ(previewed, 2u);
  ASSERT_TRUE(store.beginSession(session));
  std::vector<size_t> queue;
  buildReviewQueue(store, session, true, queue);
  EXPECT_EQ(previewed, queue.size());

  // Park the remaining two: the preview must read 0, exactly like the review
  // screen's "Nothing due".
  ASSERT_TRUE(store.recordGrade(1, AnkiGrade::Good, session, kGap));
  ASSERT_TRUE(store.recordGrade(2, AnkiGrade::Good, session, kGap));
  previewed = preview();
  EXPECT_EQ(previewed, 0u);
  ASSERT_TRUE(store.beginSession(session));
  buildReviewQueue(store, session, true, queue);
  EXPECT_EQ(previewed, queue.size());
}
