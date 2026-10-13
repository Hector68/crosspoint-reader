# Where the card pictures came from

Sourced with `scripts/fetch_test_images.py` (one word at a time: keyword
search -> download -> Pillow `L` -> `<=320x160` -> `1`-bpp BMP; the source
PNG/JPEG is deleted after conversion).

Provider used for every card: **Openverse** public API
`GET https://api.openverse.org/v1/images/?q=<keyword>&page_size=6` ->
`results[0].url` (direct image URL, no API key needed; anonymous quota
20 req/min, 200 req/day). LoremFlickr was unreachable (HTTP 000) and the
`web_search` tool's default provider (Exa) returns 403/Cloudflare, so no
keyed search provider was usable.

`apple` did not need a search: its picture was already in this folder.

| word | source URL | licence |
| --- | --- | --- |
| apple | (pre-existing `apple.png`, source unknown) | ? |
| book | https://live.staticflickr.com/2584/3703282279_756db66672_b.jpg | by-nc |
| cat | https://live.staticflickr.com/3313/3481540500_c846c62863_b.jpg | by |
| dog | https://live.staticflickr.com/8116/8606654389_e56c706e2c_b.jpg | by-nc |
| house | https://live.staticflickr.com/8842/17852177972_428a2c1060_b.jpg | by-nc-sa |
| water | https://live.staticflickr.com/2703/4464699723_7725eb5601_b.jpg | by-nc |
| bread | https://live.staticflickr.com/4032/4309415403_b32376d3aa_b.jpg | by-sa |
| sun | https://live.staticflickr.com/7147/6487199145_b4843f4cef_b.jpg | by-nc |
| car | https://live.staticflickr.com/8711/16687001674_c0659d5e69_b.jpg | by |
| phone | https://live.staticflickr.com/96/250084608_0422ae3414.jpg | by-nc-nd |
| door | https://live.staticflickr.com/6133/5990680591_c3db8d8183_b.jpg | by-nc |
| chair | https://live.staticflickr.com/3254/2730613340_13097c67ea_b.jpg | by-nc-nd |
| tree | https://live.staticflickr.com/7144/6621252273_1e533702bf_b.jpg | by-nc-sa |
| milk | https://live.staticflickr.com/3818/8921970990_872e5d653f_b.jpg | by-nc-nd |
| shoe | https://live.staticflickr.com/2099/2384595309_e7635c6bcf.jpg | by-nd |
| hat | https://live.staticflickr.com/150/385964407_480c6dc76b_b.jpg | by-nc-nd |
| ball | https://live.staticflickr.com/5302/5745797045_d036a494ae_b.jpg | by-nc-sa |
| key | https://live.staticflickr.com/5192/5836014803_4aebf85632_b.jpg | by-nc |
| pen | https://live.staticflickr.com/2612/3686088228_c757e9f754_b.jpg | by-nc-nd |
| bag | https://live.staticflickr.com/5829/22849233280_57759e3bcd_b.jpg | by |
| bird | https://live.staticflickr.com/7422/27484321144_419612b329_b.jpg | by |
| fish | https://live.staticflickr.com/8160/7577140514_5cbc9afef3_b.jpg | by-nc |
| cheese | https://live.staticflickr.com/2691/4090407169_5d9f3ff593_b.jpg | by |
| coffee | https://live.staticflickr.com/65535/15724523757_c69b31b5e8_b.jpg | by-nc |
| window | https://live.staticflickr.com/8454/8031440441_e189954b87_b.jpg | by-nd |
| table | https://live.staticflickr.com/5304/5673687239_07f6d29671_b.jpg | by-sa |
| guitar | https://live.staticflickr.com/2262/2413472242_b53479ca81.jpg | pdm |
| bicycle | https://live.staticflickr.com/3904/14375434305_402a965bd1_b.jpg | by-sa |
| mountain | https://live.staticflickr.com/7539/16129656628_ddd1db38c2_b.jpg | by-sa |
| river | https://live.staticflickr.com/3538/5803043660_c03d825d2a_b.jpg | by-sa |
| rain | https://live.staticflickr.com/3906/14674848444_e04aeab462_b.jpg | by-nc |
| star | https://live.staticflickr.com/5495/14424959091_355ba99980_b.jpg | (top hit at fetch time) |
| moon | https://live.staticflickr.com/7026/6803749855_db7a74cca5_b.jpg | by |
| clock | https://live.staticflickr.com/3546/3658873911_681bd5f0f6_b.jpg | by-sa |
| umbrella | https://live.staticflickr.com/3149/2775486094_2bacbb9be4_b.jpg | by-nc |
| To mix and match | https://live.staticflickr.com/210/495373236_eeaa5439b1_b.jpg | by-nc-sa |
| To be a fashion slave | https://live.staticflickr.com/7564/15883296589_cc599c3898.jpg | by |
| To look forward to something | https://live.staticflickr.com/7147/6496264699_7704f1859c_b.jpg | by-sa |
| To get used to something | https://live.staticflickr.com/2/1743995_7950ad9579_b.jpg | by-nc-nd |
| To be worth doing | https://live.staticflickr.com/46/139467906_b574967ce9.jpg | by-nc-nd |
| To put off something | https://live.staticflickr.com/4106/5045561700_08bd773416.jpg | by |
| To make up one's mind | https://live.staticflickr.com/5242/5302015421_20ec03f8c9_b.jpg | by-nc |

**Licence warning:** most sources are `by-nc*` (non-commercial). Fine for an
on-device *test* deck, not for anything shipped. Re-generate with
`.venv/bin/python scripts/fetch_test_images.py` and prefer CC0/PDM hits, or buy
art, before this deck goes anywhere real.
