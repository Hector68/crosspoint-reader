#include "AnkiCards.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <strings.h>  // strdup, strtok, strcasecmp

namespace {
constexpr uint32_t PROGRESS_MAGIC = 0x4b415043;  // 'CPAK'
// v2 replaced the day-based schedule with the deck-visit counter, so a v1 file
// is not readable and gets rewritten (see beginSession/recordGrade).
constexpr uint16_t PROGRESS_VERSION = 2;

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

// Writes a fresh progress file: header (carrying the session counter) plus one
// empty slot per card. Slots are reserved up front because a file cannot be
// seeked past its end, and a record is later written at a fixed offset.
bool writeProgressFile(const std::string& path, const uint32_t session, const size_t cardCount) {
  Storage.remove(path.c_str());  // a stale layout must not leave a tail behind
  HalFile created;
  if (!Storage.openFileForWrite("ANKI", path, created)) {
    LOG_ERR("ANKI", "Cannot create progress file: %s", path.c_str());
    return false;
  }
  const uint16_t recordSize = static_cast<uint16_t>(sizeof(AnkiProgressRecord));
  bool ok = tryWritePod(created, PROGRESS_MAGIC) && tryWritePod(created, PROGRESS_VERSION) &&
            tryWritePod(created, recordSize) && tryWritePod(created, session);
  const AnkiProgressRecord empty{};
  for (size_t i = 0; ok && i < cardCount; ++i) {
    ok = tryWritePod(created, empty);
  }
  created.close();
  if (!ok) LOG_ERR("ANKI", "Cannot write progress header: %s", path.c_str());
  return ok;
}

// Reads the header and reports whether it matches this build's layout. A file
// from an older version (or a truncated one) must be replaced, not appended to:
// the record offsets depend on the header size.
bool readProgressHeader(HalFile& file, uint32_t& session) {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t recordSize = 0;
  session = 0;
  return tryReadPod(file, magic) && magic == PROGRESS_MAGIC && tryReadPod(file, version) &&
         version == PROGRESS_VERSION && tryReadPod(file, recordSize) &&
         recordSize == sizeof(AnkiProgressRecord) && tryReadPod(file, session);
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

AnkiProgressRecord AnkiSrs::applyGrade(const AnkiProgressRecord& current, const AnkiGrade grade,
                                       const uint32_t session, const uint8_t minSessions) {
  AnkiProgressRecord next = current;
  // A zero gap would make the card due again immediately, defeating the point of
  // answering "I know"; the setting's own range is the source of truth.
  const uint8_t gap = std::max<uint8_t>(1u, minSessions);
  if (grade == AnkiGrade::Good) {
    // A fixed gap rather than a growing ladder: the unit is a deck visit, so a
    // growing one would hide a well-known card for months.
    next.repetitions = static_cast<uint8_t>(std::min<unsigned>(current.repetitions + 1u, 255u));
    next.dueAtSession = session + gap;
  } else if (grade == AnkiGrade::Again) {
    next.repetitions = 0;
    next.dueAtSession = session;  // due again immediately
    next.againCount = static_cast<uint8_t>(std::min<unsigned>(current.againCount + 1u, 255u));
  }
  return next;
}

bool AnkiSrs::isDue(const AnkiProgressRecord& record, const uint32_t session) {
  // dueAtSession == 0 means the card was never answered (or its record was
  // dropped because the deck text changed): always due.
  return record.dueAtSession == 0 || record.dueAtSession <= session;
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

uint32_t AnkiCardStore::storedSession(const std::string& folder) {
  HalFile file;
  if (!Storage.openFileForRead("ANKI", folder + "/progress.bin", file)) return 0;  // never reviewed
  uint32_t session = 0;
  const bool usable = readProgressHeader(file, session);
  file.close();
  // An unusable header means beginSession() will rewrite the file and start at
  // zero, so reporting 0 here keeps the preview in step with that visit.
  return usable ? session : 0;
}

uint32_t AnkiCardStore::countDueCards(const std::string& folder, const uint32_t session, const uint32_t cardCount) {
  if (cardCount == 0) return 0;

  HalFile file;
  if (!Storage.openFileForRead("ANKI", folder + "/progress.bin", file)) {
    return cardCount;  // no schedule on the card: every card is new
  }
  uint32_t stored = 0;
  if (!readProgressHeader(file, stored)) {
    file.close();
    return cardCount;  // older/truncated layout: beginSession() starts it over empty
  }

  // Records are fixed-size slots in deck order, so one sequential pass answers
  // the question — a seek per card would put a stall on the list screen.
  uint32_t due = 0;
  uint32_t read = 0;
  AnkiProgressRecord record{};
  while (read < cardCount && tryReadPod(file, record)) {
    if (AnkiSrs::isDue(record, session)) ++due;
    ++read;
  }
  file.close();
  // Slots past the end of a truncated file carry no record, so those cards are
  // as new as an unwritten slot — the review queue treats them the same way.
  return due + (cardCount - read);
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
void buildReviewQueue(const AnkiCardStore& store, const uint32_t session, const bool dueOnly,
                      std::vector<size_t>& out) {
  out.clear();
  const size_t count = store.cardCount();
  if (out.capacity() < count) out.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    AnkiProgressRecord progress{};
    store.loadProgress(i, progress);  // absent/stale record => treat as a new card
    if (!dueOnly || AnkiSrs::isDue(progress, session)) out.push_back(i);
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
  // Sized for four text fields plus an image name, the longest record the format
  // allows (2 KB on the stack, the same order as the rest of the firmware).
  char line[MAX_FIELD_BYTES * 4 + MAX_IMAGE_NAME_BYTES + 10];
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
  // them. At most five are split, which is the longest supported record.
  const char* fields[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  size_t fieldCount = 0;
  char* p = line;
  size_t remaining = length;
  while (fieldCount < 5) {
    fields[fieldCount++] = p;
    char* tab = remaining > 0 ? static_cast<char*>(memchr(p, '\t', remaining)) : nullptr;
    if (!tab) break;
    *tab = '\0';
    remaining -= static_cast<size_t>(tab - p) + 1;
    p = tab + 1;
  }
  if (fieldCount < 2) return false;  // a record without a tab is not a card

  // An optional picture always comes last, and the number of remaining text
  // fields picks the meaning:
  //   2 = word, translate              (Anki's own Front/Back export)
  //   3 = word, transcription, translate
  //   4 = word, transcription, translate, context
  // The picture is recognised by its file extension. That is what keeps Anki's
  // own three-field shape (front, back, image) apart from a three-field record
  // that carries a transcription instead.
  size_t textFields = fieldCount;
  if (looksLikeImageName(fields[fieldCount - 1])) {
    out.image = fields[fieldCount - 1];
    textFields = fieldCount - 1;
  }

  if (textFields >= 4) {
    out.word = fields[0];
    out.transcription = fields[1];
    out.translate = fields[2];
    out.context = fields[3];
  } else if (textFields == 3) {
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
      out.translate.size() > MAX_FIELD_BYTES || out.context.size() > MAX_FIELD_BYTES) {
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
  uint32_t session = 0;
  bool ok = readProgressHeader(file, session);
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

bool AnkiCardStore::beginSession(uint32_t& outSession) {
  outSession = 0;
  if (offsets_.empty()) return false;

  // Read the counter this deck has reached. A missing file, or one written by
  // an older layout, starts the deck over at zero.
  uint32_t session = 0;
  bool usable = false;
  HalFile file;
  if (Storage.openFileForRead("ANKI", progressPath(), file)) {
    usable = readProgressHeader(file, session);
    file.close();
  }
  if (!usable) {
    session = 0;
    if (!writeProgressFile(progressPath(), session, offsets_.size())) return false;
  }

  // Persist the counter for this visit before any card is shown: the whole
  // schedule hangs off it, and a reboot mid-session must not rewind it.
  session += 1;
  HalFile counter = Storage.open(progressPath().c_str(), O_RDWR);
  if (!counter) {
    LOG_ERR("ANKI", "Cannot open progress file for the session counter: %s", progressPath().c_str());
    return false;
  }
  const bool ok = counter.seek(PROGRESS_HEADER_BYTES - sizeof(uint32_t)) && tryWritePod(counter, session);
  counter.close();
  if (!ok) {
    LOG_ERR("ANKI", "Cannot persist the session counter: %s", progressPath().c_str());
    return false;
  }
  outSession = session;
  return true;
}

bool AnkiCardStore::recordGrade(const size_t index, const AnkiGrade grade, const uint32_t session,
                                const uint8_t minSessions) {
  if (index >= offsets_.size()) return false;

  AnkiCard card;
  if (!readCard(index, card)) return false;

  AnkiProgressRecord record{};
  loadProgress(index, record);
  // A record belonging to different front text (the host regenerated the deck)
  // is dropped rather than applied to the new card.
  if (record.frontHash != hashFront(card.word)) record = AnkiProgressRecord{};
  record = AnkiSrs::applyGrade(record, grade, session, minSessions);
  record.frontHash = hashFront(card.word);

  // beginSession() normally created the file already; this keeps recordGrade
  // usable on its own (the host tests call it directly).
  if (!Storage.exists(progressPath().c_str())) {
    if (!writeProgressFile(progressPath(), session, offsets_.size())) return false;
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
