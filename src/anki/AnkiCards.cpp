#include "AnkiCards.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <strings.h>  // strdup, strtok, strcasecmp

namespace {
constexpr uint32_t PROGRESS_MAGIC = 0x4b415043;  // 'CPAK'
constexpr uint16_t PROGRESS_VERSION = 1;
constexpr uint16_t INVALID_INTERVAL = 0xffffu;

// Checked POD transfer. The develop branch carries serialization::tryReadPod /
// tryWritePod, but this fork is based on the 1.6.5 tag, where
// lib/Serialization/Serialization.h only has readPod/writePod that discard the
// transfer count. The progress file needs the count, or a truncated file would
// be accepted as if it were complete.
template <typename T>
bool tryReadPod(HalFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == static_cast<int>(sizeof(T));
}

template <typename T>
bool tryWritePod(HalFile& file, const T& value) {
  return file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

// True when a field names a picture file, e.g. "img_001.bmp" (what the host
// tool writes) or "photo.jpg" (a hand-made deck). Used to tell Anki's own
// front/back/image records apart from word/transcription/translate ones: both
// carry three fields, so only the last field can disambiguate them.
bool looksLikeImageName(const char* field) {
  if (field == nullptr) return false;
  const size_t length = strlen(field);
  if (length == 0 || length > AnkiCardStore::MAX_IMAGE_NAME_BYTES) return false;
  static constexpr const char* EXTENSIONS[] = {".bmp", ".png", ".jpg", ".jpeg"};
  for (const char* extension : EXTENSIONS) {
    const size_t extensionLength = strlen(extension);
    if (length > extensionLength && strcasecmp(field + length - extensionLength, extension) == 0) return true;
  }
  return false;
}
}  // namespace

uint32_t AnkiCardStore::hashFront(const std::string& front) {
  uint32_t h = 2166136261u;
  for (const unsigned char c : front) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

uint16_t AnkiSrs::intervalForRepetitions(const uint8_t repetitions) {
  return repetitions < INTERVAL_COUNT ? INTERVAL_DAYS[repetitions] : INTERVAL_DAYS[INTERVAL_COUNT - 1];
}

AnkiProgressRecord AnkiSrs::applyGrade(const AnkiProgressRecord& current, const AnkiGrade grade,
                                       const uint32_t nowEpoch) {
  AnkiProgressRecord next = current;
  next.lastReviewed = nowEpoch;
  if (grade == AnkiGrade::Good) {
    // One step up the ladder per correct answer; the last interval repeats.
    next.repetitions = static_cast<uint8_t>(std::min<unsigned>(current.repetitions + 1u, INTERVAL_COUNT));
    next.intervalDays = intervalForRepetitions(next.repetitions);
  } else if (grade == AnkiGrade::Again) {
    next.repetitions = 0;
    next.intervalDays = 0;  // due again immediately
    next.againCount = static_cast<uint8_t>(std::min<unsigned>(current.againCount + 1u, 255u));
  }
  return next;
}

bool AnkiSrs::isDue(const AnkiProgressRecord& record, const uint32_t nowEpoch) {
  // A card that was never answered, or answered "don't know", is always due.
  if (record.intervalDays == 0 || record.lastReviewed == 0) return true;
  // A clock the host never set (no NTP before the first review) makes intervals
  // meaningless; keeping every card due is safer than locking the deck away
  // behind an interval the device cannot count.
  if (nowEpoch < AnkiCardStore::MIN_VALID_EPOCH) return true;
  const uint64_t dueAt =
      static_cast<uint64_t>(record.lastReviewed) + static_cast<uint64_t>(record.intervalDays) * 86400ull;
  return static_cast<uint64_t>(nowEpoch) >= dueAt;
}

uint32_t AnkiCardStore::countCards(const std::string& folder) {
  HalFile file;
  const std::string path = folder + "/cards.tsv";
  if (!Storage.openFileForRead("ANKI", path, file)) return 0;
  uint32_t count = 0;
  char buf[256];
  bool inLine = false;
  for (;;) {
    const int n = file.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; ++i) {
      if (buf[i] == '\n') {
        ++count;
        inLine = false;
      } else if (!inLine && buf[i] != '\r') {
        inLine = true;
      }
    }
    if (n < static_cast<int>(sizeof(buf))) break;  // short read == end of file
  }
  if (inLine) ++count;  // final record without a trailing newline
  file.close();
  return count;
}

bool AnkiCardStore::scanDecks(std::vector<AnkiDeckRef>& out) {
  out.clear();
  for (const char* rootPath : CARDS_ROOTS) {
    if (out.size() >= MAX_DECKS) break;
    scanRoot(rootPath, out);
  }
  return !out.empty();
}

void AnkiCardStore::scanRoot(const std::string& rootPath, std::vector<AnkiDeckRef>& out) {
  HalFile root;
  if (!Storage.openFileForRead("ANKI", rootPath, root)) return;  // root not on the card
  root.rewindDirectory();
  char name[MAX_IMAGE_NAME_BYTES];
  while (out.size() < MAX_DECKS) {
    HalFile entry = root.openNextFile();
    if (!entry) break;
    const size_t length = entry.getName(name, sizeof(name));
    if (length == 0 || length >= sizeof(name)) {
      LOG_ERR("ANKI", "Skipping invalid deck folder name");
      continue;
    }
    if (!entry.isDirectory()) continue;
    AnkiDeckRef ref;
    ref.title.assign(name, length);
    ref.folder = rootPath + "/" + ref.title;
    if (!Storage.exists((ref.folder + "/cards.tsv").c_str())) continue;  // folder without a deck
    out.push_back(std::move(ref));
  }
  root.close();
}

bool AnkiCardStore::hasAnyDeck() {
  for (const char* rootPath : CARDS_ROOTS) {
    HalFile root;
    if (!Storage.openFileForRead("ANKI", rootPath, root)) continue;
    root.rewindDirectory();
    char name[MAX_IMAGE_NAME_BYTES];
    bool found = false;
    while (!found) {
      HalFile entry = root.openNextFile();
      if (!entry) break;
      if (!entry.isDirectory()) continue;
      const size_t length = entry.getName(name, sizeof(name));
      if (length == 0 || length >= sizeof(name)) continue;
      if (Storage.exists((std::string(rootPath) + "/" + std::string(name, length) + "/cards.tsv").c_str())) {
        found = true;
        break;
      }
    }
    root.close();
    if (found) return true;
  }
  return false;
}

// A session is bounded by the number of cards, not by how many are due:
// 1024 * sizeof(size_t) is the worst case and is allocated once on entry.
void buildReviewQueue(const AnkiCardStore& store, const uint32_t nowEpoch, const bool dueOnly,
                      std::vector<size_t>& out) {
  out.clear();
  const size_t count = store.cardCount();
  if (out.capacity() < count) out.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    AnkiProgressRecord progress{};
    store.loadProgress(i, progress);  // absent/stale record => treat as a new card
    if (!dueOnly || AnkiSrs::isDue(progress, nowEpoch)) out.push_back(i);
  }
}

bool AnkiCardStore::open(const std::string& folder) {
  folder_ = folder;
  const auto slash = folder.find_last_of('/');
  title_ = (slash == std::string::npos) ? folder : folder.substr(slash + 1);
  offsets_.clear();
  truncated_ = false;

  HalFile file;
  if (!Storage.openFileForRead("ANKI", cardsPath(), file)) {
    LOG_ERR("ANKI", "Missing deck file: %s", cardsPath().c_str());
    return false;
  }

  // Index the byte offset of every record so readCard() needs one seek+read
  // and no card text ever has to be held in RAM at the same time. Chunked
  // read: a record-at-a-time byte scan of a large deck is far too slow on SD.
  char buf[256];
  size_t pos = 0;
  size_t lineStart = 0;
  bool inLine = false;
  for (;;) {
    const int n = file.read(buf, sizeof(buf));
    if (n <= 0) break;
    bool hitLimit = false;
    for (int i = 0; i < n; ++i) {
      if (buf[i] == '\n') {
        if (inLine) {
          if (offsets_.size() >= MAX_CARDS_PER_DECK) {
            truncated_ = true;
            hitLimit = true;
            break;
          }
          offsets_.push_back(static_cast<uint32_t>(lineStart));
          inLine = false;
        }
      } else if (!inLine && buf[i] != '\r') {
        inLine = true;
        lineStart = pos;
      }
      ++pos;
    }
    if (hitLimit) {
      LOG_ERR("ANKI", "Deck %s has more than %u cards; the rest are not shown", title_.c_str(),
              static_cast<unsigned>(MAX_CARDS_PER_DECK));
      break;
    }
    if (n < static_cast<int>(sizeof(buf))) break;  // short read == end of file
  }
  if (!truncated_ && inLine && offsets_.size() < MAX_CARDS_PER_DECK) {
    offsets_.push_back(static_cast<uint32_t>(lineStart));  // final record, no trailing newline
  }
  file.close();
  return !offsets_.empty();
}

bool AnkiCardStore::readCard(const size_t index, AnkiCard& out) const {
  out = AnkiCard{};
  if (index >= offsets_.size()) return false;

  HalFile file;
  if (!Storage.openFileForRead("ANKI", cardsPath(), file)) return false;
  if (!file.seek(offsets_[index])) {
    file.close();
    return false;
  }
  // Read the whole maximum-size record in one call: one SD read per card shown.
  char line[MAX_FIELD_BYTES * 2 + MAX_IMAGE_NAME_BYTES + 10];
  const int n = file.read(line, sizeof(line) - 1);
  file.close();
  if (n <= 0) return false;
  line[n] = '\0';

  // Cut at the record boundary: the read is sized for the worst case and may
  // have run past the end of the line into the next record.
  char* newline = static_cast<char*>(memchr(line, '\n', static_cast<size_t>(n)));
  if (newline) *newline = '\0';

  size_t length = static_cast<size_t>(strlen(line));
  while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';

  // Fields are plain text; the host tool guarantees no tabs/newlines inside
  // them. At most four are split, which is the longest supported record.
  const char* fields[4] = {nullptr, nullptr, nullptr, nullptr};
  size_t fieldCount = 0;
  char* p = line;
  size_t remaining = length;
  while (fieldCount < 4) {
    fields[fieldCount++] = p;
    char* tab = remaining > 0 ? static_cast<char*>(memchr(p, '\t', remaining)) : nullptr;
    if (!tab) break;
    *tab = '\0';
    remaining -= static_cast<size_t>(tab - p) + 1;
    p = tab + 1;
  }
  if (fieldCount < 2) return false;  // a record without a tab is not a card

  // Four fields are the canonical layout (word, transcription, translate,
  // image). Three fields are ambiguous, so the last one decides: an image file
  // name means the record is in Anki's own export shape (front, back, image —
  // what scripts/anki_import.py writes), anything else means it is
  // word/transcription/translate (a deck without a picture).
  size_t textFields = fieldCount;
  if (fieldCount == 4) {
    out.image = fields[3];  // a missing picture is written as an empty field
    textFields = 3;
  } else if (fieldCount == 3 && looksLikeImageName(fields[2])) {
    out.image = fields[2];
    textFields = 2;
  }

  if (textFields >= 3) {
    out.word = fields[0];
    out.transcription = fields[1];
    out.translate = fields[2];
  } else if (textFields == 2) {
    // No transcription column: the record is Anki's front/back shape.
    out.word = fields[0];
    out.translate = fields[1];
  } else {
    out.word = fields[0];
  }
  if (out.word.empty()) return false;

  if (out.word.size() > MAX_FIELD_BYTES || out.transcription.size() > MAX_FIELD_BYTES ||
      out.translate.size() > MAX_FIELD_BYTES) {
    LOG_ERR("ANKI", "Card %u in %s exceeds the %u-byte field limit", static_cast<unsigned>(index), title_.c_str(),
            static_cast<unsigned>(MAX_FIELD_BYTES));
  }
  return true;
}

bool AnkiCardStore::loadProgress(const size_t index, AnkiProgressRecord& out) const {
  out = AnkiProgressRecord{};
  if (index >= offsets_.size()) return false;

  HalFile file;
  if (!Storage.openFileForRead("ANKI", progressPath(), file)) return false;  // never reviewed here
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t recordSize = 0;
  bool ok = tryReadPod(file, magic) && magic == PROGRESS_MAGIC;
  ok = ok && tryReadPod(file, version) && version == PROGRESS_VERSION;
  ok = ok && tryReadPod(file, recordSize) && recordSize == sizeof(AnkiProgressRecord);
  if (ok) {
    // Fixed-size records, one per card in deck order: seek straight to the slot.
    ok = file.seek(PROGRESS_HEADER_BYTES + index * sizeof(AnkiProgressRecord)) && tryReadPod(file, out);
  }
  file.close();
  if (!ok) {
    LOG_ERR("ANKI", "Ignoring progress file %s (bad header or truncated)", progressPath().c_str());
    out = AnkiProgressRecord{};
  }
  return ok;
}

bool AnkiCardStore::recordGrade(const size_t index, const AnkiGrade grade, const uint32_t nowEpoch) {
  if (index >= offsets_.size()) return false;

  AnkiCard card;
  if (!readCard(index, card)) return false;

  AnkiProgressRecord record{};
  loadProgress(index, record);
  // A record belonging to different front text (the host regenerated the deck)
  // is dropped rather than applied to the new card.
  if (record.frontHash != hashFront(card.word)) record = AnkiProgressRecord{};
  record = AnkiSrs::applyGrade(record, grade, nowEpoch);
  record.frontHash = hashFront(card.word);

  // Create the file (header + one empty slot per card) before writing a record
  // at an offset: a file cannot be seeked past its end, so a deck whose cards
  // have never been answered must have every slot reserved up front.
  if (!Storage.exists(progressPath().c_str())) {
    HalFile created;
    if (!Storage.openFileForWrite("ANKI", progressPath(), created)) {
      LOG_ERR("ANKI", "Cannot create progress file: %s", progressPath().c_str());
      return false;
    }
    const uint16_t recordSize = static_cast<uint16_t>(sizeof(AnkiProgressRecord));
    bool ok = tryWritePod(created, PROGRESS_MAGIC) && tryWritePod(created, PROGRESS_VERSION) &&
              tryWritePod(created, recordSize);
    const AnkiProgressRecord empty{};
    for (size_t i = 0; ok && i < offsets_.size(); ++i) {
      ok = tryWritePod(created, empty);
    }
    created.close();
    if (!ok) {
      LOG_ERR("ANKI", "Cannot write progress header: %s", progressPath().c_str());
      return false;
    }
  }

  HalFile file = Storage.open(progressPath().c_str(), O_RDWR);
  if (!file) {
    LOG_ERR("ANKI", "Cannot open progress file for writing: %s", progressPath().c_str());
    return false;
  }
  if (!file.seek(PROGRESS_HEADER_BYTES + index * sizeof(AnkiProgressRecord))) {
    file.close();
    return false;
  }
  const bool ok = tryWritePod(file, record);
  file.close();
  if (!ok) LOG_ERR("ANKI", "Cannot write progress record %u", static_cast<unsigned>(index));
  return ok;
}
