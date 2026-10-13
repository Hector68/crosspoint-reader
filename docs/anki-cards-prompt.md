# Prompt: make an LLM produce a CrossPoint Anki deck

Copy everything between the horizontal rules into a model that can write files
(Claude, GPT, Gemini, …). Fill in the placeholders in the first block. The prompt
is written for the **1.6.5 Hector Edition** deck format — including the optional
`context` field and the fact that the schedule counts deck visits, not days.

This file is documentation for humans; the model only needs the prompt itself.

---

## ROLE AND TASK

You are producing a flashcard deck (Anki-compatible, plain files) for the
**CrossPoint "1.6.5 Hector Edition"** firmware running on an Xteink X3/X4 e-reader:
ESP32-C3, e-ink panel 480×800, **1 bit per pixel**, no PSRAM, no RTC.

Deck parameters:

- topic / folder name (latin letters, digits, dashes): `<FILL IN>`, e.g. `english-russian`
- what is being learned: `<FILL IN>`
- front-side language: `<FILL IN>`
- back-side language: `<FILL IN>`
- number of cards: `<FILL IN>`
- wants pictures: `<yes/no>`; if yes, style: `<FILL IN>`, e.g. simple black-and-white pictogram
- wants example sentences (`context`): `<yes/no>`

## WHAT YOU MUST DELIVER

Exactly four artifacts, in this order:

1. `build_deck.py` — a self-contained Python script (standard library only,
   Pillow optional) that CREATES:
   - `<topic>/cards.tsv`
   - `<topic>/*.bmp` (if pictures are wanted)

   Run it with `python build_deck.py --dest .`
   The script MUST write real TAB characters (U+0009), e.g. via `"\t".join(...)`,
   never spaces. This is critical: the device splits fields on tabs only.
2. The resulting `cards.tsv` content as text, for review. Show tabs as `<TAB>`
   and say so in the caption.
3. `validate.py` — a validator script (see SELF-CHECK below).
4. `README.md` — 5–10 lines: what the deck is, how to copy it to the SD card,
   how to regenerate it.

No binary data in your answer. No base64. Only code that produces it.

## HARD RULES FOR cards.tsv

Format: UTF-8 **WITHOUT a BOM**, `\n` line endings, one card per line, fields
separated by exactly one TAB (U+0009).

There are five positions. **The picture is optional and always last**, and it is
recognised by its file extension (`.bmp`, `.png`, `.jpg`, `.jpeg`, case
insensitive). Everything before it is text, and the number of text fields picks
the meaning:

| text fields | meaning | example |
| --- | --- | --- |
| 2 | `word <TAB> translate` | `cat<TAB>кошка` |
| 3 | `word <TAB> transcription <TAB> translate` | `cat<TAB>kat<TAB>кошка` |
| 4 | `word <TAB> transcription <TAB> translate <TAB> context` | `cat<TAB>kat<TAB>кошка<TAB>The cat sleeps on the sill.` |

Add an image by appending its file name as the last field:

```text
apple<TAB>AP-ul<TAB>яблоко<TAB>She put an apple in her bag.<TAB>apple.bmp
```

Consequences you must respect:

- Want transcription **and** context **and** a picture? Use all five positions,
  in that order.
- A missing transcription is written as an empty field:
  `word<TAB><TAB>translate` (3 text fields, the last one is not an image name).
- The picture must be the LAST field. If the last field is not an image file
  name, the record has no picture and that field is text.
- Never put a `.png`/`.jpg` name in the deck unless the file really is a BMP:
  the extension only marks the field as the picture, and the device then tries to
  decode a BMP and fails.

Forbidden inside a field: TAB, `\n`, `\r`. Replace them with a space. Do not add
leading or trailing spaces — the firmware does **not** trim fields, so
`apple.bmp ` (with a trailing space) becomes a file name that cannot be opened.

Limits (violating them means truncated or unreadable text):

- one field ≤ **480 bytes** in UTF-8 (not characters: Cyrillic is 2 bytes per
  letter, CJK up to 3);
- the whole line ≤ **1900 bytes**: the firmware reads one record into a
  ~2 KB buffer, and anything longer is silently cut off;
- ≤ **1024 cards** per deck, ≤ **32 decks**;
- **no empty lines** in the file (an empty line becomes an unreadable card);
- no comments, no headers, no `#separator:tab` lines — the file contains nothing
  but card records;
- the file must be named exactly `cards.tsv` (lowercase).

## WHAT THE DEVICE DOES WITH THE FIELDS

- **Front side**: `context` (if present, italic, at the top, set apart from the
  word below by a thin horizontal rule), then `word` (large), then
  `transcription` (small), then the picture.
- **Back side**: `translate`, then the picture. The context is not repeated.
- The `context` is a sentence the word appears in. It is rendered at 14 pt with
  at most 5 wrapped lines, so **one short sentence is ideal** (~10–15 words).
- Scheduling counts **deck visits**, not days: the X4 has no RTC and the system
  clock is never set, so a day-based interval would leave every card due forever.
  A card answered "I know" is parked for **N visits** (default 4, user-configurable).

## HARD RULES FOR PICTURES

- File format: **BMP, 1 bit per pixel** (monochrome, `biBitCount = 1`),
  uncompressed (`BI_RGB`), 2-colour palette.
- In a 1-bit BMP bit `1` = white (paper) and bit `0` = black (ink). Palette
  order: index 0 — black, index 1 — white.
- Rows are padded to 4 bytes: `row_bytes = ((width + 31) // 32) * 4`.
- Size: **≤ 440×480 recommended** (the panel is 480 px wide). Anything larger is
  scaled down by the firmware, which only loses quality. The decoder's hard limit
  is 2048×3072, which is meaningless for a card.
- File name: latin letters, digits, underscores; ≤ 64 bytes; extension `.bmp`.
  The name in the TSV must match the file name **byte for byte**.
- Pictures live in the SAME folder as `cards.tsv`. Subfolders do not work.
- Keep the artwork simple: pictograms, silhouettes, thick lines, high contrast.
  Half-tones on a 1-bit panel look like dirt unless dithered.

How to generate them (pick one):

- **Pillow** (preferred): draw in `L` or `RGB`, then `img.convert("1")` — that
  gives Floyd–Steinberg dithering, which looks good on e-ink. Save as BMP.
- **No dependencies**: write PBM (P4) or build the BMP header yourself. Then you
  must get the palette, the row padding and the height sign (bottom-up is fine)
  exactly right.

## FOLDER LAYOUT ON THE SD CARD

```text
/cards/<topic>/cards.tsv      <- your file
/cards/<topic>/<image>.bmp    <- your pictures
/cards/<topic>/progress.bin   <- written BY THE DEVICE, do not create it
```

The same layout also works under `/plugins/anki/cards/<topic>/`. The firmware
scans both roots. Creating `progress.bin` yourself will corrupt the schedule.

## SELF-CHECK — validate.py MUST VERIFY

Write a validator that prints PASS/FAIL and exits non-zero on any problem:

1. the file is named `cards.tsv`, reads as UTF-8, has no BOM;
2. no empty lines; every line has ≥ 2 fields;
3. no `\t`, `\n`, `\r` inside fields, and no leading/trailing whitespace;
4. every field ≤ 480 UTF-8 bytes;
5. every whole line ≤ 1900 bytes;
6. the number of fields is 2, 3, 4 or 5 (1, or ≥ 6, is FAIL);
7. if the last field looks like an image name for a 4- or 5-field record, that
   file must exist next to `cards.tsv` and be a valid BMP with `biBitCount == 1`;
8. for every referenced picture: exists, ≤ 440×480 (warn if larger), 1 bit per
   pixel, parses as BMP;
9. ≤ 1024 cards;
10. image names are ≤ 64 bytes and match the files on disk exactly.

Run the validator mentally over your own output and show the result at the end
(PASS on every point, or the list of things you fixed).

## COMMON MISTAKES TO AVOID

- Spaces instead of TABs — the most frequent one. Build the TSV in code, not by
  hand.
- A BOM at the start of the file — it corrupts the word of the first card.
- Trailing spaces in fields — the picture then "cannot be opened".
- Pointing at `.png`/`.jpg` instead of a real BMP.
- Putting the picture anywhere but last.
- Truncating by characters instead of bytes — Cyrillic and CJK blow past the
  limit.
- Comments or headers in `cards.tsv`.
- Empty lines between cards.
- A `context` that is a whole paragraph: it is rendered at 14 pt with 5 wrapped
  lines at most.

## EXAMPLES (tabs shown as `<TAB>`)

Anki's own export shape (front / back / picture):

```text
What is the capital of Japan?<TAB>Tokyo<TAB>capital_of_japan.bmp
How do you say "cat" in Japanese?<TAB>neko
```

Vocabulary with a pronunciation hint, a translation and a picture:

```text
apple<TAB>AP-ul<TAB>яблоко<TAB>apple.bmp
book<TAB>buk<TAB>книга<TAB>book.bmp
```

With an example sentence (`context`) — the full five positions:

```text
apple<TAB>AP-ul<TAB>яблоко<TAB>She put an apple in her bag.<TAB>apple.bmp
```

With `context` but no picture (four text fields):

```text
rain<TAB>rain<TAB>дождь<TAB>It started to rain before we left.
```

Without a transcription and without a picture (two fields):

```text
cat<TAB>кошка
```

With an empty transcription (three text fields):

```text
hello<TAB><TAB>привет
```

## OPTIONAL: COMING FROM ANKI DESKTOP

If the source material is an Anki collection rather than something you invent,
the person on the other side can export it and convert it on their computer:

1. Anki desktop → **File → Export** → *Notes in Plain Text* (`.txt`/`.tsv`),
   fields **Front**, **Back** (put any artwork in the same folder and name the
   files in a third field).
2. Convert it with the firmware repository's host tool, which cleans the HTML,
   truncates fields and converts the artwork to 1-bit BMP:

   ```sh
   .venv/bin/python scripts/anki_import.py --input export.tsv \
       --topic my-deck --dest /Volumes/CROSSPOINT
   ```

   (`--image-max` defaults to `440x480`; Pillow is required for pictures.)
3. Eject the card, insert it, and open **Home → Anki cards → my-deck**.

Your job, when the user gives you raw notes instead, is to produce the same
result directly: a `cards.tsv` plus the pictures, following the rules above.

Start your answer with a plan: the list of cards (word → translation), then
`build_deck.py`, then the `cards.tsv` content, then `validate.py`, then
`README.md`. If pictures are wanted, draw a simple pictogram for each one in code
— never try to inline binary data.
