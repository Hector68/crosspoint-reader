# Anki cards on the device

Anki decks are **plain files on the SD card**. The firmware never parses
`.apkg`, SQLite, JPEG or PNG — a host-side tool converts an Anki export into
the small formats the ESP32-C3 can read directly. This keeps the review path
working on the Xteink X4 (no PSRAM, ~380 KB RAM).

## Deck layout on the SD card

```text
/cards/<topic>/cards.tsv      one card per line, TAB-separated fields:
                              front<TAB>back[<TAB>image]
                              or, with a pronunciation hint:
                              word<TAB>transcription<TAB>translate[<TAB>image]
                              or the full form, with an example sentence:
                              word<TAB>transcription<TAB>translate<TAB>context[<TAB>image]
/cards/<topic>/img_001.bmp    pre-rendered 1bpp images (last field)
/cards/<topic>/progress.bin   SRS state, written by the device
```

`/plugins/anki/cards/<topic>/…` is scanned as a second root, so a deck can also
be delivered as an SD plugin folder (see `sd-plugins/anki/`) without any PC
step:

```sh
cp -R sd-plugins/anki fs_/plugins/anki   # simulator SD root; /Volumes/<card> on a real reader
```

`sd-plugins/anki/cards` ships three decks: `english-japanese` (three fields,
one picture), `english-russian` (four fields, one picture per card) and
`english-context` (the five-field form with an example sentence, plus one card
that has no picture). All of them appear in the deck list after that copy. Decks
are **indexed, never loaded**: only the byte offset of every line is kept in RAM
(4 bytes per card, max 1024 cards/deck, 32 decks).

`cards.tsv` is exactly Anki's own export shape — fields separated by TABs, one
record per line — with two extra rules the host tool enforces:

* a field must not contain a TAB, a newline or a carriage return (they are
  replaced with spaces);
* a field must not exceed 480 bytes (truncated at a UTF-8 boundary).

How the fields are read: the **last** field is the picture when it names a
`.bmp`/`.png`/`.jpg` file. Everything before it is text, and the number of text
fields picks the layout — two are `word<TAB>translate` (Anki's own Front/Back
export), three are `word<TAB>transcription<TAB>translate`, and four are
`word<TAB>transcription<TAB>translate<TAB>context`. The picture is optional and
always last, so the full five-field form is
`word<TAB>transcription<TAB>translate<TAB>context<TAB>image`. The extension is
what tells Anki's own three-field shape (`front<TAB>back<TAB>img.bmp`, no
pronunciation hint) apart from a three-field record that carries a transcription
instead.

The `context` field is a sentence the word appears in. It is shown on the front
side only, in italic at the top of the card, separated from the word below by a
thin inset rule. It is rendered with the same limit as the other text fields, so
one short sentence (10–15 words) is the right size.

On the review screen the front shows, from the top: the `context` sentence when
the card has one (italic, above a thin inset rule), then the word (plus the
pronunciation hint when there is one) and the picture; the back shows the
translation and the picture. The context is not repeated on the back, and the
front never shows the translation.

To have a model write a deck from scratch instead of exporting one, use the
copy-paste prompt in [anki-cards-prompt.md](anki-cards-prompt.md).

### Why BMP and not PNG

The panel is 1 bit per pixel (the X4 has an 800x480 1-bit framebuffer; its
4-level gray mode is a separate pair of planes the review screen does not use),
so a 1-bit BMP is exactly what the panel can show — a PNG would carry no extra
detail, only the need to decode it. The host tool already dithers with
Pillow's `convert("1")` (Floyd-Steinberg), which is what makes the artwork look
smooth without any gray levels.

Quality therefore comes from **resolution**, not the container: convert at the
largest size the card area can show (`--image-max`, default `440x480`). The
review screen scales a too-large picture down to the space left after the text,
so a deck converted at that size is never cropped or dropped.

The firmware does have a PNG decoder (`PngToBmpConverter`, used for EPUB covers
and TXT images), so PNG decks would work — but it would put an inflate + image
decode on every card flip on an ESP32-C3, which is exactly what the "no JPEG/PNG
on the review path" design avoids.

## Importing from Anki (host tool)

1. Anki desktop → **File → Export** → note type *Basic* → *Plain Text* →
   fields *Front*, *Back* → export (call it `export.tsv`).
   If the note type has an image field, put the image files in the same folder
   as the TSV; the third field is the image file name.
2. Run:
   ```sh
   .venv/bin/python scripts/anki_import.py --input /tmp/anki_in/export.tsv \
       --topic english-japanese --dest fs_
   ```
   `--dest` is the SD card mount (or `fs_` for the simulator). `--image-max`
   caps the image size (default `440x480` — the review screen is 480 px wide and
   about 660 px tall, so this leaves a margin without shrinking the artwork);
   `--make-sample-image` generates one placeholder image so a text-only machine
   can still ship a deck with a picture. Images are resized and converted to
   **1-bit BMP** (needs Pillow; without it such records are reported as skipped,
   never half-converted).
3. Eject/remount the card (or restart the simulator) and the deck appears in
   the home menu as **Anki cards** → *<topic>*.

`.apkg`/`.colpkg` files are also accepted: the tool unzips them, reads
`collection.anki2` (SQLite, fields separated by `\x1f`) and pulls `<img src>`
references out of the field text into the third field.

## Using it on the device

Home → **Anki cards** (this slot replaced *OPDS Browser*; it is only shown when
a deck exists — with no decks the slot falls back to *Plugins*, then *OPDS*) →
pick a deck → review.

| button | action |
| --- | --- |
| Back | leave the deck |
| Confirm | flip the card (front ↔ back) |
| Right | “I know” (grades the card *Good*) |
| Left | “I don't know” (grades the card *Again*) |

The last row of the deck list is **Browse All**, which opens the plugin
catalog, so OPDS/plugins stay reachable.

## Scheduling (device side, counted in deck visits)

`progress.bin` starts with a 12-byte header — `magic u32`, `version u16` (= 2),
`recordSize u16`, **`sessionCounter u32`** — followed by one fixed 12-byte record
per card:

| field | type | meaning |
| --- | --- | --- |
| `frontHash` | u32 | FNV-1a/32 of the front text |
| `dueAtSession` | u32 | deck visit this card becomes due again; 0 = never scheduled |
| `repetitions` | u8 | consecutive “I know” answers |
| `againCount` | u8 | “I don't know” answers |
| `reserved` | u16 | padding, keeps the record 12 bytes and 4-byte aligned |

**Why visits and not days.** The Xteink X4 has no RTC chip, and nothing in the
firmware seeds the system clock from one (the I2C `Rtc` is a PCF8563 the X4 does
not populate, and `HalClock::syncFromNTP()` returns early without it, so
`configTzTime()` never runs). `time()` therefore never reaches a real date, and a
day-based interval would leave every card permanently due. Counting deck visits
needs no clock at all and survives reboots and deep sleep, because the counter
lives on the SD card.

* one **session** = one entry into a deck; the counter is bumped and persisted by
  `AnkiCardStore::beginSession()` before the first card is shown;
* “I know” parks the card until `session + N`, where N is the Settings value
  (Settings → **Reader** → *Anki: pause "I know" cards*, default 4, range 1–30);
  the gap is **fixed**, not a growing ladder — with visits as the unit a growing
  one would hide a well-known card for months;
* “I don't know” sets `dueAtSession = session`, so the card is due immediately
  and also comes back at the end of the current session;
* a card is *due* when `dueAtSession == 0` (never answered) or
  `dueAtSession <= session`;
* **Review all** (Confirm on the *Review complete* screen) deliberately ignores
  `dueAtSession` and lists every card, which is how a deck is drilled out of turn;
* if the counter cannot be written, the screen falls back to showing every card
  rather than hiding cards behind a schedule the device cannot keep;
* progress is keyed by `frontHash`, so if the host regenerates a deck with
  different front text the stale record is discarded instead of being applied
  to a different card;
* a `progress.bin` from the older day-based layout (version 1, 8-byte header) is
  **replaced** on the next deck entry: the record offsets depend on the header
  size, so such a file must never be appended to.

## Firmware changes / how to re-apply after an upstream update

Everything is on branch `feature/anki-cards` (base `c1a62f66`, develop HEAD —
chosen because the SD-plugin system did not exist at tag 1.6.5). The change set
is deliberately additive:

* new files: `src/anki/AnkiCards.{h,cpp}`,
  `src/activities/anki/Anki{DeckList,Review}Activity.{h,cpp}`,
  `scripts/anki_import.py`, `sd-plugins/anki/`, `docs/anki-cards.md`;
* touched files: `src/activities/ActivityManager.{h,cpp}` (menu item +
  `goToAnki()`), `src/activities/home/HomeActivity.{h,cpp}` (the library slot),
  `lib/I18n/translations/english.yaml` (new `STR_*` keys);
* new i18n keys: `STR_ANKI_CARDS`, `STR_NO_CARDS_FOUND`, `STR_N_CARDS`,
  `STR_FLIP`, `STR_KNOW`, `STR_DONT_KNOW`, `STR_CARD_N_OF_M`,
  `STR_DECK_NOT_FOUND`, `STR_REVIEW_COMPLETE`, `STR_CARD_UNREADABLE`,
  `STR_IMAGE_TOO_LARGE`, `STR_REVIEW_ALL`, `STR_NOTHING_DUE`,
  `STR_ANKI_MIN_SESSIONS`, `STR_ANKI_MIN_SESSIONS_VALUE_FORMAT` (English only;
  other languages fall back to English until the keys are translated).

To re-apply on a newer upstream tree: copy the *new files* back as-is, then
re-make the small edits listed above (the diff is small and localised; the
`HomeMenuItem` enum keeps `OPDS_BROWSER` as an alias of `ANKI_CARDS` so the
existing OPDS code compiles unchanged). Build and check with:

```sh
.venv/bin/pio run -e simulator            # builds the firmware + simulator
.venv/bin/pio run -e simulator -t run_simulator
```
