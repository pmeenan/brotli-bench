# Corpora sources

Real-world benchmark inputs. Every regular top-level file in `corpora/` that
does not start with `.` and does not end in `.md` is auto-discovered by the
harness. `corpora/dictionary/` holds compression-dictionary-transport cases
(see [`dictionary/manifest.tsv`](dictionary/manifest.tsv)).

Everything here can be re-downloaded / regenerated with
[`scripts/fetch_corpora.sh`](../scripts/fetch_corpora.sh); checksums live in
[`scripts/corpora.sha256`](../scripts/corpora.sha256) (kept outside `corpora/`
so the harness does not pick it up). Files were fetched on 2026-10-29.

## Top-level corpora

| Path | Source URL | Version | License (SPDX) |
| --- | --- | --- | --- |
| `jquery-3.7.1.min.js` | https://cdnjs.cloudflare.com/ajax/libs/jquery/3.7.1/jquery.min.js | 3.7.1 | MIT |
| `jquery-3.7.1.js` | https://cdnjs.cloudflare.com/ajax/libs/jquery/3.7.1/jquery.js | 3.7.1 | MIT |
| `react-dom-18.3.1.production.min.js` | https://cdnjs.cloudflare.com/ajax/libs/react-dom/18.3.1/umd/react-dom.production.min.js | 18.3.1 | MIT |
| `lodash-4.17.21.min.js` | https://cdnjs.cloudflare.com/ajax/libs/lodash.js/4.17.21/lodash.min.js | 4.17.21 | MIT |
| `vue-3.4.38.global.prod.js` | https://cdnjs.cloudflare.com/ajax/libs/vue/3.4.38/vue.global.prod.js | 3.4.38 | MIT |
| `three-r128.min.js` | https://cdnjs.cloudflare.com/ajax/libs/three.js/r128/three.min.js | r128 | MIT |
| `moment-2.30.1.min.js` | https://cdnjs.cloudflare.com/ajax/libs/moment.js/2.30.1/moment.min.js | 2.30.1 | MIT |
| `bootstrap-5.3.3.min.css` | https://cdnjs.cloudflare.com/ajax/libs/bootstrap/5.3.3/css/bootstrap.min.css | 5.3.3 | MIT |
| `font-awesome-6.5.2.all.min.css` | https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.5.2/css/all.min.css | 6.5.2 (Free) | MIT (CSS code only; no icons/fonts included) |
| `foundation-6.8.1.min.css` | https://cdnjs.cloudflare.com/ajax/libs/foundation/6.8.1/css/foundation.min.css | 6.8.1 | MIT |
| `wiki-Brotli.html` | https://en.wikipedia.org/w/index.php?title=Brotli&oldid=1382373437 | oldid 1382373437 | CC-BY-SA-4.0 |
| `wiki-Huffman_coding.html` | https://en.wikipedia.org/w/index.php?title=Huffman_coding&oldid=1379018579 | oldid 1379018579 | CC-BY-SA-4.0 |
| `wiki-Espresso.html` | https://en.wikipedia.org/w/index.php?title=Espresso&oldid=1381363121 | oldid 1381363121 | CC-BY-SA-4.0 |

## Dictionary-transport files (`corpora/dictionary/`)

| Path | Source URL | Version | License (SPDX) |
| --- | --- | --- | --- |
| `dictionary/jquery-3.6.4.min.js` | https://cdnjs.cloudflare.com/ajax/libs/jquery/3.6.4/jquery.min.js | 3.6.4 | MIT |
| `dictionary/react-dom-18.2.0.production.min.js` | https://cdnjs.cloudflare.com/ajax/libs/react-dom/18.2.0/umd/react-dom.production.min.js | 18.2.0 | MIT |
| `dictionary/lodash-4.17.20.min.js` | https://cdnjs.cloudflare.com/ajax/libs/lodash.js/4.17.20/lodash.min.js | 4.17.20 | MIT |
| `dictionary/vue-3.3.13.global.prod.js` | https://cdnjs.cloudflare.com/ajax/libs/vue/3.3.13/vue.global.prod.js | 3.3.13 | MIT |
| `dictionary/bootstrap-5.3.2.min.css` | https://cdnjs.cloudflare.com/ajax/libs/bootstrap/5.3.2/css/bootstrap.min.css | 5.3.2 | MIT |
| `dictionary/wiki-html.dict` | Generated (see below) from Wikipedia pages | - | CC-BY-SA-4.0 (derived from Wikipedia text) |
| `dictionary/manifest.tsv` | Hand-written case list | - | - |

## Wikipedia attribution

The `wiki-*.html` files are the full rendered desktop HTML (Vector skin) of
English Wikipedia articles, fetched by revision id with User-Agent
`brotli-bench/0.1 (pmeenan@google.com)`. Article text is by Wikipedia
contributors, licensed under the
[Creative Commons Attribution-ShareAlike 4.0 License](https://creativecommons.org/licenses/by-sa/4.0/)
(CC BY-SA 4.0); the page histories list the authors. The files are unmodified.
Rendered HTML (skin, banners, `load.php` URLs) can drift even for a fixed
`oldid`, so the committed copies are canonical.

| File | Article | oldid | History (authors) |
| --- | --- | --- | --- |
| `wiki-Brotli.html` | "Brotli" | 1382373437 | https://en.wikipedia.org/w/index.php?title=Brotli&action=history |
| `wiki-Huffman_coding.html` | "Huffman coding" | 1379018579 | https://en.wikipedia.org/w/index.php?title=Huffman_coding&action=history |
| `wiki-Espresso.html` | "Espresso" | 1381363121 | https://en.wikipedia.org/w/index.php?title=Espresso&action=history |

### `dictionary/wiki-html.dict` generation

- Tool: `vendor/brotli-c/research/dictionary_generator` (brotli-c @ `d89c4b1`),
  built out-of-tree with `g++ -O2` against libdivsufsort
  (`y-256/libdivsufsort@5f60d6f`, MIT). The `deorummolae` engine is stubbed
  out (it needs esaxx, which is not vendored).
- Command: `dictionary_generator --dsh -t64K wiki-html.dict train/wiki-*.html`
  (`durchschlag` engine, target 64 KiB, defaults `slice_len=16`,
  `block_len=1024`, `min_slice_pop=2`, no chunking). Result: 65,256 bytes.
  Deterministic for identical inputs. `--dsh` beat `--sieve` by ~4-8% on the
  held-out pages.
- Training set (20 pages, total 16.9 MB, **not committed**; fetched to a temp
  dir by the script), English Wikipedia, CC BY-SA 4.0, title (oldid):
  Albert_Einstein (1382671920), Amazon_River (1382171474),
  C_(programming_language) (1382678438), CSS (1382423783),
  Data_compression (1382027270), Deflate (1377071705),
  Google_Chrome (1381749616), Gzip (1373208344), HTML (1382636951),
  HTTP (1377007807), JavaScript (1379385036), Jazz (1381387713),
  Linux (1381784225), LZ77_and_LZ78 (1374388117),
  Photosynthesis (1379238357), Python_(programming_language) (1382352332),
  Rust_(programming_language) (1381627603), Unicode (1382629125),
  World_Wide_Web (1379635447), Zstd (1382272412).
- Held-out inputs (not in the training set): `wiki-Brotli.html`,
  `wiki-Huffman_coding.html`, `wiki-Espresso.html`.

## Compression ratios

Measured with the C `brotli` CLI built from `vendor/brotli-c` (`d89c4b1`,
v1.2.0), one-shot file compression with CLI defaults (window auto-sized to the
input), round-trip verified. Sizes are bytes of the brotli stream.

| File | Size (B) | q5 (B) | q5 ratio | q11 (B) | q11 ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| `bootstrap-5.3.3.min.css` | 232803 | 28250 | 8.24x | 22709 | 10.25x |
| `font-awesome-6.5.2.all.min.css` | 103009 | 22047 | 4.67x | 18938 | 5.44x |
| `foundation-6.8.1.min.css` | 136326 | 16121 | 8.46x | 14282 | 9.55x |
| `jquery-3.7.1.js` | 285314 | 79680 | 3.58x | 69545 | 4.10x |
| `jquery-3.7.1.min.js` | 87533 | 29764 | 2.94x | 27446 | 3.19x |
| `lodash-4.17.21.min.js` | 73015 | 25220 | 2.90x | 23089 | 3.16x |
| `moment-2.30.1.min.js` | 58912 | 18374 | 3.21x | 16955 | 3.47x |
| `react-dom-18.3.1.production.min.js` | 131835 | 41037 | 3.21x | 37180 | 3.55x |
| `three-r128.min.js` | 603445 | 136810 | 4.41x | 120830 | 4.99x |
| `vue-3.4.38.global.prod.js` | 146843 | 52172 | 2.81x | 48085 | 3.05x |
| `wiki-Brotli.html` | 238523 | 37216 | 6.41x | 32904 | 7.25x |
| `wiki-Espresso.html` | 457879 | 69005 | 6.64x | 59751 | 7.66x |
| `wiki-Huffman_coding.html` | 284541 | 47902 | 5.94x | 41869 | 6.80x |

### Dictionary cases

Same CLI, with and without `-D <dictionary>` (raw LZ77 dictionary), round-trip
verified.

| Case | Input (B) | Dict (B) | q5 no dict | q5 dict | q11 no dict | q11 dict |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `jquery-3.6.4-to-3.7.1.min.js` | 87533 | 89795 | 29764 (2.9x) | 6981 (12.5x, -77%) | 27446 (3.2x) | 5010 (17.5x, -82%) |
| `react-dom-18.2.0-to-18.3.1.production.min.js` | 131835 | 131882 | 41037 (3.2x) | 3048 (43.3x, -93%) | 37180 (3.5x) | 2796 (47.2x, -92%) |
| `lodash-4.17.20-to-4.17.21.min.js` | 73015 | 72805 | 25220 (2.9x) | 6954 (10.5x, -72%) | 23089 (3.2x) | 5581 (13.1x, -76%) |
| `vue-3.3.13-to-3.4.38.global.prod.js` | 146843 | 133165 | 52172 (2.8x) | 33210 (4.4x, -36%) | 48085 (3.1x) | 28605 (5.1x, -41%) |
| `bootstrap-5.3.2-to-5.3.3.min.css` | 232803 | 232948 | 28250 (8.2x) | 193 (1206x, -99%) | 22709 (10.3x) | 190 (1225x, -99%) |
| `wiki-html-Brotli` | 238523 | 65256 | 37216 (6.4x) | 25107 (9.5x, -33%) | 32904 (7.2x) | 22371 (10.7x, -32%) |
| `wiki-html-Huffman_coding` | 284541 | 65256 | 47902 (5.9x) | 35659 (8.0x, -26%) | 41869 (6.8x) | 31348 (9.1x, -25%) |
| `wiki-html-Espresso` | 457879 | 65256 | 69005 (6.6x) | 55768 (8.2x, -19%) | 59751 (7.7x) | 48630 (9.4x, -19%) |

> Note: bootstrap 5.3.2 -> 5.3.3 is almost byte-identical, so that case is
> effectively a "copy the whole dictionary" stress test (very long matches
> into the dictionary); vue 3.3 -> 3.4 is the opposite extreme (large code
> changes between minor versions, so the dictionary helps much less).

## Candidates considered and rejected

- **Bulma** (MIT): 677KB, compresses 15-19x. That is as unrealistically
  compressible as the old synthetic corpora.
- **normalize.css** (MIT): about 6KB, too small to measure throughput
  meaningfully.
- **Project Gutenberg HTML:** a full book is large, and the brotli testdata
  (`alice29.txt`, `lcet10.txt`, `asyoulik.txt`) already covers plain text.
- **Larger Wikipedia articles:** most are 0.6-2.8MB. The three chosen pages
  (238-458KB) keep verify/bench time reasonable.
