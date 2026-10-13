#!/usr/bin/env python3
"""Turn an Anki export into a CrossPoint card deck (see docs/anki-cards.md).

Anki desktop: File -> Export -> "Notes in Text File" gives exactly the input
this tool expects: one record per line, fields separated by TABs
(front, back[, image]).  What this tool adds is the adaptation to the device:

  * strip the HTML Anki leaves in the fields (Anki stores rich text)
  * make sure no field contains a TAB or a newline, because the firmware reads
    the file line by line without a parser
  * convert each record's image to the device's 1bpp BMP format, sized to fit
    the screen, and rewrite the field to just the file name
  * write /cards/<topic>/cards.tsv (+ the images) into --dest, which is either
    an SD card mount or the simulator's fs_ folder

Usage:
  python3 scripts/anki_import.py --input export.tsv --topic english-japanese
  python3 scripts/anki_import.py --input deck.apkg --topic japanese-food --dest fs_
  python3 scripts/anki_import.py --input export.tsv --topic demo --make-sample-image

Pillow is optional: it is used to convert JPEG/PNG artwork to 1bpp BMP.  Without
it, text-only decks still import; records that need converted artwork are
reported as skipped instead of being half-converted.
"""

import argparse
import html
import json
import re
import sqlite3
import sys
import tempfile
import zipfile
from pathlib import Path

MAX_FIELD_BYTES = 480  # keep in sync with AnkiCardStore::MAX_FIELD_BYTES
TAG_RE = re.compile(r"<[^>]*>")
IMG_RE = re.compile(r"<img[^>]*src=[\"']?([^\"'>]+)[\"']?[^>]*/?>", re.I)
BREAK_RE = re.compile(r"<br\s*/?>|</?(div|p|li|tr)[^>]*>", re.I)


def clean(text: str) -> str:
    """Strip Anki's HTML and force the value into one TAB-free line."""
    text = html.unescape(text)
    text = BREAK_RE.sub(" ", text)
    text = TAG_RE.sub("", text)
    for ch in ("\t", "\n", "\r"):
        text = text.replace(ch, " ")
    return " ".join(text.split())


def truncate_utf8(text: str, limit: int) -> str:
    """Cut to `limit` bytes without splitting a UTF-8 sequence."""
    raw = text.encode("utf-8")
    if len(raw) <= limit:
        return text
    while raw:
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            raw = raw[:-1]
    return ""


def convert_image(src: Path, dst: Path, max_w: int, max_h: int) -> bool:
    """Re-scale and convert artwork into the 1bpp BMP the device blits directly."""
    try:
        from PIL import Image  # optional dependency (requirements.txt)
    except ImportError:
        return False
    try:
        with Image.open(src) as img:
            img.load()
            if img.mode not in ("1", "L", "RGB", "RGBA"):
                img = img.convert("RGB")
            width, height = img.size
            scale = min(max_w / max(1, width), max_h / max(1, height), 1.0)
            if scale < 1.0:
                img = img.resize((max(1, round(width * scale)), max(1, round(height * scale))),
                                 Image.Resampling.LANCZOS)
            img.convert("1").save(dst, format="BMP")
    except Exception as error:  # unreadable/corrupt artwork must not abort the import
        print(f"  cannot convert {src}: {error}", file=sys.stderr)
        return False
    return True


def make_placeholder(dst: Path) -> bool:
    """Write a valid 1bpp BMP (a black-bordered box) with no third-party deps,
    so a sample deck can ship with an image even on a machine without Pillow."""
    width, height = 320, 160
    row_bytes = width // 8
    pixels = bytearray(b"\xff" * (row_bytes * height))  # 1bpp: a set bit is paper
    for x in range(width):  # top and bottom edges
        pixels[x // 8] &= ~(0x80 >> (x % 8))
        pixels[(height - 1) * row_bytes + x // 8] &= ~(0x80 >> (x % 8))
    for y in range(height):  # left and right edges
        pixels[y * row_bytes] &= 0x7F
        pixels[y * row_bytes + row_bytes - 1] &= 0xFE if row_bytes > 1 else 0x7F
    data = bytes(pixels)
    header = bytearray(b"BM")
    header += (14 + 40 + 8 + len(data)).to_bytes(4, "little")
    header += b"\x00\x00" + b"\x00\x00"
    header += (14 + 40 + 8).to_bytes(4, "little")
    header += (40).to_bytes(4, "little") + width.to_bytes(4, "little") + height.to_bytes(4, "little")
    header += (1).to_bytes(2, "little") + (1).to_bytes(2, "little")  # planes, bpp
    header += (0).to_bytes(4, "little") + len(data).to_bytes(4, "little")
    header += (3780).to_bytes(4, "little") * 2  # ~96 DPI
    header += (2).to_bytes(4, "little") + (0).to_bytes(4, "little")
    header += bytes([0, 0, 0, 0]) + bytes([0xFF, 0xFF, 0xFF, 0])  # ink, paper
    dst.write_bytes(bytes(header) + data)
    return True


def records_from_tsv(path: Path):
    try:
        handle = path.open("r", encoding="utf-8-sig", newline="")
    except OSError as error:
        print(f"cannot read {path}: {error}", file=sys.stderr)
        return
    with handle:
        for number, line in enumerate(handle, start=1):
            line = line.rstrip("\r\n")
            if not line.strip() or line.startswith("#"):
                continue  # Anki's "#separator:tab" / "#html:true" header lines
            fields = line.split("\t")
            if len(fields) < 2:
                print(f"  skipping record {number}: expected front<TAB>back", file=sys.stderr)
                continue
            image = path.parent / fields[2] if len(fields) > 2 else None
            yield number, fields[0], fields[1], image


def records_from_apkg(path: Path):
    """Anki's .apkg/.colpkg: a zip holding a SQLite collection + media files."""
    tmp = Path(tempfile.mkdtemp(prefix="anki_import_"))
    try:
        with zipfile.ZipFile(path) as archive:
            archive.extractall(tmp)
    except (zipfile.BadZipFile, OSError) as error:
        print(f"cannot unpack {path}: {error}", file=sys.stderr)
        return
    collection = next(tmp.glob("*.anki2*"), None)
    if collection is None:
        print(f"  no collection.anki2 file inside {path}", file=sys.stderr)
        return
    media = {}
    media_file = next(tmp.glob("media*"), None)
    if media_file:
        try:
            media = json.loads(media_file.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError) as error:
            print(f"  ignoring unreadable media map: {error}", file=sys.stderr)
    try:
        conn = sqlite3.connect(str(collection))
        rows = conn.execute("select flds from notes order by id").fetchall()
        conn.close()
    except sqlite3.Error as error:
        print(f"  cannot read {collection}: {error}", file=sys.stderr)
        return
    media_dir = tmp / "collection.media"
    for number, (flds,) in enumerate(rows, start=1):
        fields = flds.split("\x1f")
        if len(fields) < 2:
            print(f"  skipping note {number}: fewer than 2 fields", file=sys.stderr)
            continue
        # Anki keeps artwork as <img src="name"> inside a field. Pull it out of
        # the text and point the device's third field at the extracted file.
        image = None
        match = IMG_RE.search(fields[1]) or IMG_RE.search(fields[0])
        if match:
            name = match.group(1)
            candidate = media_dir / name
            if not candidate.exists() and name.isdigit():
                candidate = media_dir / str(media.get(name, ""))
            if candidate.exists():
                image = candidate
        fields[0] = IMG_RE.sub("", fields[0])
        fields[1] = IMG_RE.sub("", fields[1])
        yield number, fields[0], fields[1], image


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert an Anki export (TSV or .apkg) into a CrossPoint card deck.",
        epilog="See the module docstring for the expected input format.",
    )
    parser.add_argument("--input", required=True, help="Anki export: TSV or .apkg/.colpkg")
    parser.add_argument("--topic", default="demo", help="deck folder name created on the SD card")
    parser.add_argument("--dest", default="fs_", help="SD root (the simulator uses ./fs_)")
    # The review screen is 480 px wide with ~660 px of content height, so this
    # leaves the artwork a margin without shrinking it.
    parser.add_argument("--image-max", default="440x480", help="max image size in pixels, WxH")
    parser.add_argument("--make-sample-image", action="store_true",
                        help="give the first card a generated sample image (no Pillow needed)")
    args = parser.parse_args()

    src = Path(args.input).expanduser()
    if not src.exists():
        print(f"input not found: {src}", file=sys.stderr)
        return 2
    width, _, height = args.image_max.partition("x")
    max_w, max_h = int(width), int(height or width)

    topic = re.sub(r"[^A-Za-z0-9_-]+", "-", args.topic).strip("-") or "demo"
    deck = Path(args.dest).expanduser() / "cards" / topic
    deck.mkdir(parents=True, exist_ok=True)

    if src.suffix.lower() in (".apkg", ".colpkg", ".zip"):
        records = list(records_from_apkg(src))
    else:
        records = list(records_from_tsv(src))

    out_lines = []
    warnings = 0
    for number, front, back, image in records:
        trimmed_front = truncate_utf8(clean(front), MAX_FIELD_BYTES)
        trimmed_back = truncate_utf8(clean(back), MAX_FIELD_BYTES)
        if not trimmed_front:
            print(f"  skipping record {number}: empty front", file=sys.stderr)
            warnings += 1
            continue
        if len(trimmed_front.encode()) != len(clean(front).encode()) or \
           len(trimmed_back.encode()) != len(clean(back).encode()):
            print(f"  record {number}: a field was truncated to {MAX_FIELD_BYTES} bytes", file=sys.stderr)
            warnings += 1
        fields = [trimmed_front, trimmed_back]
        if image is not None:
            name = f"img_{len(out_lines) + 1:03d}.bmp"
            if convert_image(Path(image), deck / name, max_w, max_h):
                fields.append(name)
            else:
                print(f"  record {number}: image {image} not converted (install Pillow)", file=sys.stderr)
                warnings += 1
        if args.make_sample_image and not out_lines:
            name = "img_001.bmp"
            make_placeholder(deck / name)
            fields.append(name)
        out_lines.append("\t".join(fields))

    tsv = deck / "cards.tsv"
    tsv.write_text("".join(f"{line}\n" for line in out_lines), encoding="utf-8")
    print(f"OK: wrote {len(out_lines)} cards to {tsv}")
    if warnings:
        print(f"   ({warnings} warning(s), see above)", file=sys.stderr)
    if not out_lines:
        return 1
    return 2 if warnings else 0


if __name__ == "__main__":
    sys.exit(main())
