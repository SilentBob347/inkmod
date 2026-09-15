<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/inkmod-logo-dark.png">
    <source media="(prefers-color-scheme: light)" srcset="assets/inkmod-logo-light.png">
    <img alt="inkMOD — Custom Firmware for Xteink X3, X4 and X4 Pro"
         src="assets/inkmod-logo-light.png" width="300">
  </picture>
</p>

<h1 align="center">inkMOD</h1>

<p align="center">
Open-source custom firmware for <b>Xteink X3, X4 and X4 Pro</b>
</p>

<p align="center">
FB2 / FB2.ZIP / EPUB · dictionaries · custom fonts · OPDS · statistics · web tools · touch support on X4 Pro
</p>

> [!WARNING]
> Installing custom firmware always carries risk. Use the firmware image for the **correct device family**, keep a known-good recovery image, and do not erase factory calibration/NVS unless a recovery procedure explicitly requires it. The project and its contributors are not responsible for damaged or unusable devices.

## Supported devices

| Device | MCU | Main input | Hardware notes | Release image |
| --- | --- | --- | --- | --- |
| **Xteink X3** | ESP32-C3 | Physical buttons | Hardware RTC; supported display revisions | `firmware-x3x4-vX.Y.Z.bin` |
| **Xteink X4** | ESP32-C3 | Physical buttons | Supported SSD1677 / UC8179 / UC8279 revisions | `firmware-x3x4-vX.Y.Z.bin` |
| **Xteink X4 Pro** | ESP32-S3 + PSRAM | Capacitive touch + Home key | GT911 touch, warm/cold frontlight, USB mass-storage support | `firmware-x4pro-vX.Y.Z.bin` |

**Do not flash the X3/X4 image to X4 Pro or the X4 Pro image to X3/X4.** The firmware also performs device-family checks where possible, but the correct image should always be selected before flashing.

## What inkMOD includes

### Reading and book formats

- Native **FB2** and **FB2.ZIP** support.
- **EPUB** reading with browser-side EPUBKIT preparation/optimization.
- **TXT, XTC and XTCH** support.
- ZIP content detection for supported books instead of relying only on the file extension.
- Streaming and memory-conscious processing for large books.
- Improved FB2 structure and typography: annotations, epigraphs, headings, subtitles, quotations, poems, stanzas, authors and `<emphasis>`.
- Custom reader fonts, hyphenation and Unicode fallback.
- Bookmarks, text clippings, table of contents, search and configurable reader menu.
- Logical chapter/page counting across internally split large chapters.

### X4 Pro touch interface

X4 Pro is a first-class supported target, not a button-only compatibility build.

- Direct touch navigation throughout Home, settings, file browser, Wi-Fi, OPDS and reader screens.
- Reader page turning by tap or swipe, with configurable touch mode.
- Long-press text selection and dictionary lookup.
- Touch keyboard and direct row/button activation.
- Left-edge **Back** gesture and capacitive Home-key integration.
- Touch Control Center with frontlight brightness/warmth, night mode, refresh and reader-touch controls.
- Touch-aware bookmarks, statistics, dictionary, clipping selection and dialogs.
- Lyra Carousel supports short horizontal swipes that begin on the visible book cover; Home menu taps keep their normal behaviour.
- Recent-books list uses direct row touch/long-press.
- The built-in easter egg includes on-screen touch controls.

### Library and files

- File browser with folders, search and book/file information.
- **The last browser directory and selected item are remembered**, so returning from a book no longer starts at the top of the library again. This behaviour is shared by **X3, X4 and X4 Pro**.
- OPDS browsing, caching and downloads.
- Recent books and multiple Home layouts: Lyra, Lyra Carousel, RoundedRaff, Minimal and Dashboard.
- PNG/BMP handling for images and sleep screens.

### Dictionaries

- Dictionary lookup from the reader.
- Multiple installed dictionaries.
- StarDict support, including synonym tables.
- Fast paginated dictionary articles.
- Improved punctuation and line-break-hyphen lookup.
- Browser-side dictionary preparation and upload.

### Web interface

- File and directory upload/management.
- Directory-tree upload and rename support.
- Browser-side **EPUBKIT** optimizer.
- Browser-side preparation for EPUB, FB2 and supported ZIP books.
- Dictionary preparation/upload.
- Sleep-screen generator.
- PNG transparency/background processing.
- `.ttf` / `.otf` to `.cpfont` conversion and upload.

### Sleep screens and statistics

- Current-book cover, calendar and custom sleep images.
- Separate timeout sleep-screen behaviour and Quick Resume.
- Reading-time, session and per-book statistics.
- Completed-book tracking.
- KOReader progress synchronization / Quick Sync.

### Reliability and recovery

- Diagnostics screen with memory, storage, reset reason and firmware/device information.
- Crash breadcrumbs and `/crash_report.txt` for guarded failures.
- Factory-calibration based X3/X4 panel-revision selection.
- Emergency SD-card recovery through `inkmod-recovery.bin` on supported X3/X4 builds.
- Separate release binaries for ESP32-C3 (X3/X4) and ESP32-S3 (X4 Pro).

See [`RECOVERY.md`](RECOVERY.md) before experimenting with firmware recovery.

## Current release

The source tree is currently versioned as **inkMOD 1.1.8**.

Highlights of 1.1.8 include:

- full X4 Pro target and touch/frontlight integration;
- separate X3/X4 and X4 Pro release artifacts;
- X4 Pro touch navigation polish across Home, reader, settings and utility screens;
- short cover-based swipe navigation in Lyra Carousel;
- remembered file-browser directory and selection on all supported models;
- OPDS/cache, KOReader sync and browser-side book-preparation improvements;
- continued FB2/EPUB rendering and stability work.

Detailed history is in [`CHANGELOG.md`](CHANGELOG.md). Release-specific notes are in [`RELEASE_NOTES_1.1.8.md`](RELEASE_NOTES_1.1.8.md).

## Building from source

inkMOD uses **PlatformIO**.

```bash
python -m pip install -U platformio
pip install -r requirements.txt
```

### X3 / X4

```bash
# Developer build with serial logging
pio run -e x3x4-developer

# Production build
pio run -e x3x4-release

# Flash developer build through USB
pio run -e x3x4-developer -t upload
```

### X4 Pro

```bash
# Developer build with serial logging
pio run -e x4pro-developer

# Production build
pio run -e x4pro-release

# Flash developer build through USB
pio run -e x4pro-developer -t upload
```

The default PlatformIO environment is `x3x4-release`.

## GitHub Releases

A `vX.Y.Z` tag builds and publishes both hardware families automatically:

- `firmware-x3x4-vX.Y.Z.bin` — X3 / X4
- `firmware-x4pro-vX.Y.Z.bin` — X4 Pro
- `firmware-release-vX.Y.Z.bin` — compatibility alias for older X3/X4 OTA clients

CI also builds both developer and release targets on pushes and pull requests.

## Project structure

```text
src/           inkMOD application code
lib/           reader/rendering and shared libraries
freeink-sdk/   hardware abstraction, display/input/network support
web/           on-device web interface sources
scripts/       build, release and generation helpers
assets/        project artwork
.github/       CI, release automation and funding metadata
```

## Community and support

inkMOD is free and open source. Financial support is optional and never unlocks firmware functionality.

- 📢 Telegram: https://t.me/inkmodx4
- ❤️ Support development: https://send.monobank.ua/jar/9p1oM8v2sa

Installation instructions and community experience are also maintained in the inkMOD/Xteink discussion on 4PDA.

## Credits and upstream

inkMOD is based on and derived from **CrossPoint / FreeInk** work and includes third-party open-source components. See [`THIRD_PARTY.md`](THIRD_PARTY.md) and [`LICENSE`](LICENSE) for licensing information.

Contributors and testers who helped shape recent inkMOD releases include **Alpa4hinO** and **olimo**, along with community members who supplied books, logs, hardware tests and bug reports.

## License

See [`LICENSE`](LICENSE).
