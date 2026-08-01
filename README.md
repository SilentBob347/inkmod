# inkMOD

A community mod of the inkMOD e-reader firmware, with added Russian and Ukrainian
interface localization, extra format support, and a set of UI/reliability fixes.

## Features

- **Localization**: full Russian and Ukrainian interface support.
- **Formats**: added support for `.fb2` and `.fb2.zip`, on top of the base firmware's formats.
- **Themes**: multiple home-screen themes (Lyra, Lyra Carousel, RoundedRaff, Minimal,
  Dashboard), tuned for correctness and performance.
- **Fonts**: an interface text-size toggle (large vs. default) alongside the reader's
  own font settings.
- **Converters**:
  - Built-in web-based FB2 → EPUB converter.
  - `.ttf` / `.otf` → `.cpfont` font conversion, with automatic upload to the reader.
  - Web upload supports whole folders (with subfolders), converting files on the fly.
- **Clock & time sync**:
  - X4 (no hardware RTC): time is synced over Wi-Fi on connect and in the background
    on boot. If no known network is reachable, the device now falls back to the last
    successfully synced time instead of losing it entirely (see Changelog below).
  - X3 (hardware RTC): unchanged.
  - The clock can be turned off entirely in Settings if you don't need it — note this
    also disables the Calendar sleep screen, which depends on a working clock.
- **Sleep screen**: added a "Calendar" wallpaper with automatic Wi-Fi time sync, plus
  an inverted (dark) variant.
- **Reading stats**: detailed per-weekday reading statistics, matching the X3 firmware.
- **OTA updates**: firmware update checks now point at this repository's GitHub
  Releases and are built/published automatically via GitHub Actions on every tagged
  version (see [Releasing](#releasing) below).

## Changelog

### v1.0.3

- **Landscape orientation**: fixed the on-screen control-button strip overlapping list
  content and header titles across the app (Settings, Language, Font, Network mode,
  KOReader sync, OPDS, button remap, recent books, bookmarks, file actions,
  confirmation dialogs) when the screen is rotated to landscape. These screens now
  use the same orientation-aware safe-area layout the reader already used, instead of
  always assuming a portrait-only bottom button bar.
- **Clock sync screen**: fixed "Current time: 20:4" (last digit missing) after a
  successful sync — the display buffer was 1 byte too small for the multi-byte UTF-8
  translation of "Current time:" plus the time string, so `snprintf` silently
  truncated it.
- **Clock kerning bug, fixed at the root**: certain digit pairs (e.g. "3"+"4",
  "3"+"5") had bad kerning-table data that pulled the second digit far enough left to
  visually vanish behind the first. Previously patched only for the header clock;
  now fixed for every on-screen digit pair (dates, stats, page numbers, battery %, …)
  by skipping kerning between two ASCII digits at the renderer level.
- **RoundedRaff**: fixed broken/missing Cyrillic letters (и, ш, л, е, я and others)
  when "Interface text size" is set to Large. That mode used a separate LexendDeca
  14pt font with incomplete Cyrillic glyph coverage in this build; it now uses the
  same Inter 12pt bump every other theme uses, which has full Cyrillic coverage.
- **EPUB image optimization**: fixed recompressed images (e.g. `.png` → `.jpg`)
  disappearing entirely from the book after running "Optimize" on upload. The
  optimizer's image-reference rewriter only ran when the chapter HTML parsed as
  strict XHTML; content using HTML-style tags (e.g. `<img src="...">` without a
  self-closing `/>`) failed that parse and silently skipped the rewrite, so `<img>`
  tags kept pointing at the old (now-renamed) filename. Added a plain-text regex
  fallback so image references still get fixed even when strict parsing fails.
- **Reader stability on image-heavy books**: some large, heavily-illustrated books
  (e.g. children's encyclopedias) could reboot the device while reading, with no
  error shown. Root cause: this build compiles with C++ exceptions disabled, so any
  failed memory allocation anywhere in the app — not just the image-loading paths
  already guarded by memory checks — silently aborted and rebooted with nothing in
  the log. Added a global out-of-memory handler that logs the heap state before
  restarting (so a future case like this is diagnosable from one log capture), and
  widened the memory headroom required before loading another inline image, so the
  existing "skip further images under memory pressure" fallback kicks in earlier —
  before the surrounding text/CSS parsing has a chance to exhaust the remainder.

### v1.0.2

- **Reader font shortcut**: the power-button "Change font" action (short- and
  long-press) used to cycle a legacy built-in font list that no longer exists in
  this firmware — it would show "Indexing" and re-paginate the page, but the visible
  font never actually changed. It now cycles through the SD-card fonts you've
  actually installed (Settings → Font), matching what the shortcut is supposed to
  do. Requires at least two fonts installed on the SD card to see a visible change.

### v1.0.1

- **Lyra Carousel**: fixed a bug where a side book cover would intermittently render
  as a blank white square instead of its thumbnail (or the fallback silhouette) while
  scrolling through recent books, caused by a missing bitmap validity check.
- **RoundedRaff**: fixed the header clock digits smearing/sliding on e-ink partial
  refreshes — the header area is now fully cleared before each redraw.
- **All themes**: fixed a font-kerning bug where certain digit pairs (e.g. "3"+"4",
  "3"+"5") could cause the clock to visually drop its last digit (e.g. "11:34" showing
  as "11:3"). The clock now renders glyph-by-glyph, which also keeps its measured and
  rendered width in sync.
- **Sleep screen**: added an inverted (dark) variant of the Calendar sleep screen,
  selectable from Settings.
- **X4 time sync**: the device now persists the last successfully NTP-synced time and
  seeds the software clock from it on boot if no Wi-Fi network is reachable to get a
  fresh sync. Previously, every real power-off wiped the clock completely and the
  Calendar sleep screen would have nothing to show; now it shows the last known time
  instead (which may be stale if the device has been offline a while, but no longer
  disappears).
- **Ukrainian localization**: fixed several awkward/incorrect machine-translated
  strings and inconsistent capitalization in the Settings menu (sleep-screen cover
  filter, wallpaper, etc.).
- **OTA updates**: update checks now point at this repository; firmware is built and
  published automatically via GitHub Actions whenever a version tag is pushed.

## Building

Requires [PlatformIO](https://platformio.org/) (VS Code extension or CLI).

```bash
pip install -r requirements.txt   # once, into PlatformIO's own Python env
pio run -e tiny                   # build
pio run -e tiny -t upload         # build + flash over USB
```

## Releasing

```bash
./scripts/release.sh 1.0.2   # bumps inkmod_version in platformio.ini, commits, tags
git push
git push origin v1.0.2
```

Pushing the tag triggers `.github/workflows/release.yml`, which builds the `tiny`
environment and publishes `firmware-tiny-v1.0.2.bin` as a GitHub Release. Devices
running this firmware will find it automatically via Settings → Update.

A community mod of the inkMOD e-reader firmware, with added Russian and Ukrainian interface localization, extra format support, and a set of UI/reliability fixes.

Features
Localization: full Russian and Ukrainian interface support.
Formats: added support for .fb2 and .fb2.zip, on top of the base firmware's formats.
Themes: multiple home-screen themes (Lyra, Lyra Carousel, RoundedRaff, Minimal, Dashboard), tuned for correctness and performance.
Fonts: an interface text-size toggle (large vs. default) alongside the reader's own font settings.
Converters:
Built-in web-based FB2 → EPUB converter.
.ttf / .otf → .cpfont font conversion, with automatic upload to the reader.
Web upload supports whole folders (with subfolders), converting files on the fly.
Clock & time sync:
X4 (no hardware RTC): time is synced over Wi-Fi on connect and in the background on boot. If no known network is reachable, the device now falls back to the last successfully synced time instead of losing it entirely (see Changelog below).
X3 (hardware RTC): unchanged.
The clock can be turned off entirely in Settings if you don't need it — note this also disables the Calendar sleep screen, which depends on a working clock.
Sleep screen: added a "Calendar" wallpaper with automatic Wi-Fi time sync, plus an inverted (dark) variant.
Reading stats: detailed per-weekday reading statistics, matching the X3 firmware.
OTA updates: firmware update checks now point at this repository's GitHub Releases and are built/published automatically via GitHub Actions on every tagged version (see Releasing below).
Changelog
v1.0.1
Lyra Carousel: fixed a bug where a side book cover would intermittently render as a blank white square instead of its thumbnail (or the fallback silhouette) while scrolling through recent books, caused by a missing bitmap validity check.
RoundedRaff: fixed the header clock digits smearing/sliding on e-ink partial refreshes — the header area is now fully cleared before each redraw.
All themes: fixed a font-kerning bug where certain digit pairs (e.g. "3"+"4", "3"+"5") could cause the clock to visually drop its last digit (e.g. "11:34" showing as "11:3"). The clock now renders glyph-by-glyph, which also keeps its measured and rendered width in sync.
Sleep screen: added an inverted (dark) variant of the Calendar sleep screen, selectable from Settings.
X4 time sync: the device now persists the last successfully NTP-synced time and seeds the software clock from it on boot if no Wi-Fi network is reachable to get a fresh sync. Previously, every real power-off wiped the clock completely and the Calendar sleep screen would have nothing to show; now it shows the last known time instead (which may be stale if the device has been offline a while, but no longer disappears).
Ukrainian localization: fixed several awkward/incorrect machine-translated strings and inconsistent capitalization in the Settings menu (sleep-screen cover filter, wallpaper, etc.).
OTA updates: update checks now point at this repository; firmware is built and published automatically via GitHub Actions whenever a version tag is pushed.
