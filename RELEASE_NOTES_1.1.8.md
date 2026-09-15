# inkMOD 1.1.8 — X4 Pro support and touch/navigation polish

inkMOD 1.1.8 adds **Xteink X4 Pro** as a first-class supported device while keeping full support for Xteink X3 and X4.

## X4 Pro

- Separate ESP32-S3 / PSRAM build target and release image.
- GT911 capacitive touch integration and capacitive Home-key handling.
- Warm/cold frontlight control and touch Control Center.
- USB mass-storage support path.
- Direct touch navigation across Home, settings, File Browser, Wi-Fi, OPDS, dictionaries, statistics, bookmarks and reader menus.
- Reader tap/swipe page controls, long-press selection and touch keyboard.
- Touch-friendly dialogs and utility screens that previously depended on physical buttons.

## Home and navigation

- Lyra Carousel can be moved with a **short horizontal drag starting directly on a visible book cover**.
- Home menu touch targets keep their existing behaviour and are not consumed by carousel gestures.
- Recent Books list touch hitboxes now match the real rendered two-line row height.
- The easter egg can be entered, confirmed and controlled entirely by touch on X4 Pro.

## File Browser

- The File Browser remembers the **last directory and selected item**.
- Returning to the library after reading no longer forces you to search from the top of a large series or folder.
- This behaviour is shared by **X3, X4 and X4 Pro** and survives reboot through `/.inkmod/file_browser_position.bin`.

## Release images

Use the image for the correct device family:

- `firmware-x3x4-v1.1.8.bin` — Xteink X3 / X4
- `firmware-x4pro-v1.1.8.bin` — Xteink X4 Pro
- `firmware-release-v1.1.8.bin` — compatibility alias for older X3/X4 OTA clients

Do **not** cross-flash the ESP32-C3 and ESP32-S3 images.

## Notes

This release builds on 1.1.7, which already included Quick Sync, OPDS caching/download improvements, browser-side preparation for EPUB/FB2/ZIP books, and the related FB2/EPUB stability fixes.
