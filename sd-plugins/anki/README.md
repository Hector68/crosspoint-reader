# Anki cards (SD plugin)

This folder is the **Anki → CrossPoint** bridge. The firmware never parses
`.apkg`, SQLite, JPEG or PNG — everything is converted on the computer, so the
review path fits in the ~380 KB of RAM on the Xteink X4.

## What the device expects

```text
cards/<topic>/cards.tsv     one card per line:  front<TAB>back[<TAB>image]
                            or with a pronunciation hint:
                            word<TAB>transcription<TAB>translate[<TAB>image]
cards/<topic>/img_001.bmp   pre-rendered 1bpp images (last field)
cards/<topic>/progress.bin  written by the device (SRS state)
```

Copy this whole folder onto an SD card as `plugins/anki/` (or `/.crosspoint/plugins/anki/`)
and the decks inside it show up automatically — no firmware rebuild needed.
The firmware scans `/cards` first and `/plugins/anki/cards` second.

`cards.tsv` is Anki's own export shape (TAB-separated, one record per line),
with two device limits: a field may not contain a TAB/newline, and may not be
longer than 480 bytes.

The **last** field is the picture when it names a `.bmp`/`.png`/`.jpg` file.
That is how the device tells `front<TAB>back<TAB>image` (this folder's
`english-japanese` deck) from `word<TAB>transcription<TAB>translate` (a deck
with no pictures); `english-russian` uses the four-field form. The front of the
card shows the word (and the pronunciation hint), the back shows the
translation and the picture.

Pictures are 1-bit BMP because that is exactly what the panel shows (1 bit per
pixel); the host tool dithers them with Pillow, so quality comes from the
converted size, not from the file format. Convert at `--image-max 440x480` (the
default) — the review screen scales a larger picture down to the space left
after the card text, so nothing is cropped or dropped.

## How to add your own deck

1. Anki desktop → **File → Export** → note type *Basic* → **Plain Text** →
   fields *Front*, *Back* → save as `export.tsv`.
   For cards with a picture, add a third field with the image file name and put
   that image next to the TSV.
2. On the computer that can see the SD card:
   ```sh
   python scripts/anki_import.py --input export.tsv --topic my-topic --dest /Volumes/CROSSPOINT
   ```
   (Use `--dest fs_` for the simulator.) Images are resized to `--image-max`
   (default `440x480`) and converted to 1-bit BMP; that step needs Pillow
   (`pip install -r requirements.txt`).
3. Re-insert the card. Home → **Anki cards** → pick the topic.

A ready-made deck is included under `cards/english-japanese/` (3 cards, one
with an image); `sample-export.tsv` next to it shows the input format.

## On the device

| button | action |
| --- | --- |
| Back | leave the deck |
| Confirm | flip the card |
| Right | “I know” |
| Left | “I don't know” |

The last row of the deck list is **Browse All** → the plugin catalog, so
OPDS/plugins remain reachable.
