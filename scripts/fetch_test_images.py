#!/usr/bin/env python3
"""Build the Anki test deck: word / transcription / translate / image.

The firmware never decodes JPEG or PNG, so every picture is downloaded and
converted to a 1bpp bitmap here (host side).

Finding a picture (one word at a time, first usable hit wins):
  1. an image already sitting in the deck folder (``apple.png``);
  2. a keyword search on an image search provider - Openverse
     (https://api.openverse.org/v1/images/?q=<keyword>) and, as a second
     provider, the Wikimedia Commons API.  Both are key-less and were chosen
     because LoremFlickr is unreachable from this network (HTTP 000) and the
     pi ``web_search`` tool's default provider (Exa) answers 403/Cloudflare.
     Candidate image URLs are pulled from the JSON and downloaded with
     urllib; the bytes are written straight to disk, never into an LLM
     context.
  3. LoremFlickr / picsum random photo as the last-resort generic fallback.

A candidate is only accepted when Pillow can decode it, it is big enough to
survive the 440x480 downscale, and it is neither blank nor a placeholder
error image (checked by the dark-pixel ratio).

Record layout (TAB separated, no TABs or newlines inside a field):
    word<TAB>transcription<TAB>translate<TAB>image
A record whose picture could not be fetched is written with three fields, so
the review screen simply shows text.

Usage:  .venv/bin/python scripts/fetch_test_images.py [--dest sd-plugins/anki/cards/english-russian]
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

MAX_W = 440          # review screen is 480 px wide; this keeps a small margin
MAX_H = 480          # ~660 px of content height, minus the card text
UA = "Mozilla/5.0 (compatible; crosspoint-test-dictionary/1.0)"
MIN_INK, MAX_INK = 0.01, 0.99  # reject blank / fully-inverted sources

# word | transcription (ASCII only: the built-in fonts have no IPA glyphs) | translate | photo keyword
WORDS = """apple|AP-ul|яблоко|apple
book|buk|книга|book
cat|kat|кошка|cat
dog|dog|собака|dog
house|haus|дом|house
water|WO-tuh|вода|water
bread|bred|хлеб|bread
sun|sun|солнце|sun
car|kar|машина|car
phone|foun|телефон|phone
door|dor|дверь|door
chair|chea|стул|chair
tree|tri|дерево|tree
milk|milk|молоко|milk
shoe|shu|ботинок|shoe
hat|hat|шляпа|hat
ball|bol|мяч|ball
key|ki|ключ|key
pen|pen|ручка|pen
bag|bag|сумка|bag
bird|bard|птица|bird
fish|fish|рыба|fish
cheese|chiiz|сыр|cheese
coffee|KO-fi|кофе|coffee
window|WIN-duh|окно|window
table|TEI-buh|стол|table
guitar|gi-TAR|гитара|guitar
bicycle|BAI-si-kl|велосипед|bicycle
mountain|MAUN-tin|гора|mountain
river|RIV-er|река|river
rain|rein|дождь|rain
star|star|звезда|star
moon|mun|луна|moon
clock|klok|часы|clock
umbrella|am-BREL-uh|зонт|umbrella
To mix and match|miks end matsh|Комбинировать, сочетать|mismatched clothes
To be a fashion slave|bi a FASH-an sleiv|Быть рабом моды|fashion
To look forward to something|luk FOR-ward|Ждать с нетерпением|waiting
To get used to something|get yust|Привыкнуть к чему-либо|routine
To be worth doing|bi werth|Стоить того|money
To put off something|put of|Откладывать на потом|sleeping
To make up one's mind|meik ap|Решиться, принять решение|thinking"""

# Search providers tried in order.  Each entry: (name, [query suffixes]).
# The query is "<keyword><suffix>", falling back to "<word><suffix>".
SEARCH_PROVIDERS = ["openverse", "wikimedia-commons"]
CANDIDATES_PER_QUERY = 6

# Last-resort generic photos (keyword ignored), kept from the original script.
PHOTO_SOURCES = [  # first hit wins; each entry is a format string for the keyword
    "https://loremflickr.com/{w}/{h}/{kw}?lock={n}",
    "https://picsum.photos/{w}/{h}?random={n}",
]

MIME_OK = {
    "image/jpeg": "jpg", "image/jpg": "jpg", "image/png": "png", "image/gif": "gif",
    "image/webp": "webp", "image/tiff": "tif", "image/bmp": "bmp",
}
EXT_OK = {"jpg", "jpeg", "png", "gif", "webp", "tif", "tiff", "bmp"}


def slugify(text: str) -> str:
    keep = [c if c.isalnum() and c.isascii() else "_" for c in text.lower()]
    slug = "".join(keep).strip("_")
    while "__" in slug:
        slug = slug.replace("__", "_")
    return slug or "card"


def open_url(url: str, timeout: int = 25):
    """urlopen restricted to http(s).

    Every URL here comes from a search result rather than a literal, so the
    scheme is checked once in this helper instead of at each call site (ruff
    S310 cannot see through the check, hence the one noqa below).
    """
    if urllib.parse.urlsplit(url).scheme.lower() not in ("http", "https"):
        raise ValueError(f"unsupported URL scheme: {url!r}")
    request = urllib.request.Request(url, headers={"User-Agent": UA})  # noqa: S310 - scheme checked above
    return urllib.request.urlopen(request, timeout=timeout)  # noqa: S310 - scheme checked above


def http_json(url: str):
    try:
        with open_url(url) as response:
            return json.loads(response.read().decode("utf-8", "replace"))
    except Exception as exc:  # noqa: BLE001 - network/HTTP/timeout/JSON
        print(f"  ! search {url} -> {exc}", file=sys.stderr)
        return None
    finally:
        time.sleep(3.2)  # key-less providers rate-limit (Openverse: 20 req/min)


def openverse_queries(query: str) -> list[str]:
    data = http_json(
        "https://api.openverse.org/v1/images/?q=" + urllib.parse.quote(query)
        + f"&page_size={CANDIDATES_PER_QUERY}&mature_filtered=true"
    ) or {}
    urls = []
    for item in data.get("results", []):
        url = item.get("url") or ""
        ext = (item.get("extension") or url.rsplit(".", 1)[-1]).lower()
        if url.startswith("https://") and ext in EXT_OK:
            urls.append(url)
    return urls


def commons_queries(query: str) -> list[str]:
    data = http_json(
        "https://commons.wikimedia.org/w/api.php?action=query&generator=search"
        "&gsrsearch=" + urllib.parse.quote(query)
        + f"&gsrnamespace=6&gsrlimit={CANDIDATES_PER_QUERY}"
        "&prop=imageinfo&iiprop=url|mime&format=json"
    ) or {}
    urls = []
    for page in (data.get("query") or {}).get("pages", {}).values():
        for info in page.get("imageinfo", []):
            url, mime = info.get("url", ""), (info.get("mime") or "").lower()
            if url.startswith("https://") and mime in MIME_OK:
                urls.append(url)
    return urls


PROVIDER_QUERIES = {"openverse": openverse_queries, "wikimedia-commons": commons_queries}


def fetch_candidate(url: str, tmp: Path) -> bool:
    """Download one candidate image; keep it only if Pillow accepts it and it
    is big enough / not blank.  Bytes go straight to disk, never to stdout."""
    try:
        from PIL import Image  # optional dependency (requirements.txt)
    except ImportError:  # pragma: no cover - Pillow lives in .venv only
        print("  ! Pillow is required to inspect images", file=sys.stderr)
        return False
    try:
        with open_url(url) as response:
            data = response.read()
    except Exception as exc:  # noqa: BLE001
        print(f"  ! {url} -> {exc}", file=sys.stderr)
        return False
    if len(data) < 600:  # a real photo is never this small; error pages are
        print(f"  ! {url} -> body too small ({len(data)} bytes)", file=sys.stderr)
        return False
    try:
        with Image.open(io.BytesIO(data)) as image:
            image.load()
            gray = image.convert("L")
            if gray.width < 200 or gray.height < 120:
                print(f"  ! {url} -> too small {gray.width}x{gray.height}", file=sys.stderr)
                return False
            histogram = gray.histogram()
            dark = sum(histogram[:128])
            total = sum(histogram) or 1
            if not (MIN_INK <= dark / total <= MAX_INK):
                print(f"  ! {url} -> blank/uniform image", file=sys.stderr)
                return False
    except Exception as exc:  # noqa: BLE001
        print(f"  ! {url} -> not an image ({exc})", file=sys.stderr)
        return False
    tmp.write_bytes(data)
    return True


def optimise(src: Path, dest: Path) -> bool:
    """Open -> convert('L') -> scale to <=440x480 -> convert('1') -> BMP, rm src."""
    try:
        from PIL import Image  # optional dependency (requirements.txt)
    except ImportError:  # pragma: no cover - Pillow lives in .venv only
        print(f"  ! cannot optimise {src}: Pillow is required", file=sys.stderr)
        return False
    try:
        with Image.open(src) as image:
            image.load()
            image = image.convert("L")  # the panel shows black and white anyway
            width, height = image.size
            if width > MAX_W or height > MAX_H:
                scale = min(MAX_W / width, MAX_H / height)
                image = image.resize(
                    (max(1, int(width * scale)), max(1, int(height * scale))), Image.Resampling.LANCZOS
                )
            image.convert("1").save(dest, "BMP")  # 1bpp: the firmware just blits it
    except Exception as exc:  # noqa: BLE001
        print(f"  ! cannot optimise {src}: {exc}", file=sys.stderr)
        return False
    with contextlib.suppress(OSError):
        src.unlink()  # the device cannot decode PNG/JPEG, so drop the source
    return True


def fetch_and_optimise(url: str, tmp: Path, dest: Path) -> bool:
    """Download one candidate and convert it in place; True when dest landed."""
    return fetch_candidate(url, tmp) and optimise(tmp, dest)


def main() -> int:
    try:  # fail fast: every image step below needs Pillow
        import PIL  # noqa: F401
    except ImportError:
        print("Pillow is required (pip install -r requirements.txt)", file=sys.stderr)
        return 2

    parser = argparse.ArgumentParser()
    parser.add_argument("--dest", default="sd-plugins/anki/cards/english-russian")
    args = parser.parse_args()

    dest_dir = Path(args.dest)
    dest_dir.mkdir(parents=True, exist_ok=True)
    tmp_dir = Path(__file__).resolve().parent.parent / ".cache" / "fetch_test_images"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    rows: list[str] = []
    missing: list[str] = []
    provenance: list[str] = []

    for number, line in enumerate(WORDS.strip().splitlines(), start=1):
        word, transcription, translate, keyword = (part.strip() for part in line.split("|"))
        image_name = slugify(word) + ".bmp"
        print(f"[{number:02d}] {word}")
        has_image = False

        # 1) artwork already placed in the deck folder (e.g. apple.png)
        for existing in (dest_dir / (slugify(word) + ".png"), dest_dir / (slugify(word) + ".jpg")):
            if existing.exists() and optimise(existing, dest_dir / image_name):
                provenance.append(f"{word}: pre-existing {existing.name}")
                has_image = True
                break

        # 2) search providers, one word at a time
        if not has_image:
            for provider in SEARCH_PROVIDERS:
                query_fn = PROVIDER_QUERIES[provider]
                for query in dict.fromkeys([keyword, word, keyword + " " + word]):  # keyword first
                    for url in query_fn(query)[:CANDIDATES_PER_QUERY]:
                        if fetch_and_optimise(url, tmp_dir / (image_name + ".src"), dest_dir / image_name):
                            provenance.append(f"{word}: {provider} <- {url}")
                            has_image = True
                            break
                    if has_image:
                        break
                if has_image:
                    break

        # 3) generic keyword photo, last resort
        if not has_image:
            for source in PHOTO_SOURCES:
                url = source.format(w=MAX_W, h=MAX_H, kw=urllib.parse.quote(keyword), n=number)
                if fetch_and_optimise(url, tmp_dir / (image_name + ".src"), dest_dir / image_name):
                    provenance.append(f"{word}: fallback {url}")
                    has_image = True
                    break

        if not has_image:
            missing.append(word)
            rows.append(f"{word}\t{transcription}\t{translate}")  # no image field
        else:
            rows.append(f"{word}\t{transcription}\t{translate}\t{image_name}")

    (dest_dir / "cards.tsv").write_text("".join(row + "\n" for row in rows), encoding="utf-8")
    print(f"deck: {dest_dir}  cards: {len(rows)}  without image: {len(missing)}")
    for line in provenance:
        print("  " + line)
    if missing:
        print("missing: " + ", ".join(missing), file=sys.stderr)
    return 0 if not missing else 1


if __name__ == "__main__":
    sys.exit(main())
