# inkMOD 1.1.9 — X4 Pro bug-fix source drop

This source package applies the ten fixes from the X4 Pro test report.

1. **Power wake on X4 Pro** — a normal short Power press now wakes the sleeping reader even when sleep is assigned to long Power. Frontlight restore is deferred until wake verification to prevent a flash on rejected wakes.
2. **Swipe vs Back** — the Back gesture must begin in the leftmost 5% of the screen; ordinary right swipes remain page-turn gestures.
3. **Home button settings from reader** — the Home submenu is now built and handled by `ControlsOptionsActivity` when settings are opened from a book.
4. **Reader menu and full-screen tap** — added `Open reader menu` as a configurable Power/Home shortcut, bottom-edge upward swipe opens the reader menu, and a new full-screen tap mode turns the page forward on any tap.
5. **Chapter wording** — `Select chapter` is now `Contents` / `Оглавление` / `Зміст`.
6. **Line-spacing controls on X4 Pro** — side PageBack/PageForward buttons adjust the fine step (1% for line spacing); the touch UI no longer mentions nonexistent arrow buttons and shows a side-button hint for percent sliders.
7. **USB text clipping** — the connected USB instruction is split into two centered lines so it fits the X4 Pro screen.
8. **X4 Pro battery** — X4 Pro now uses the board-configured CW2017 fuel gauge instead of the X4 ADC path; CW2017 profile initialisation retries after a failed attempt and rejects invalid SOC values; idle CPU downclocking is enabled on S3 at a PSRAM-safe 80 MHz while the existing rail shutdown/deep-sleep path remains in use.
9. **Large font collections** — `/api/fonts` now streams bounded JSON chunks instead of constructing a full ArduinoJson document plus serialized `String`, reducing heap fragmentation/OOM on X3/X4 with many fonts.
10. **RU/UK keyboard** — `х` is present in both layouts. The remaining Russian `ё/ъ/э` and Ukrainian `є/ї/ґ` are available as long-press alternatives on keys 1/2/3, preserving large touch targets.

Version in `platformio.ini` is bumped to **1.1.9**.

## Validation performed

- i18n tables regenerated successfully with `scripts/gen_i18n.py`.
- whitespace/error check on the source diff passed.
- static assertions/checks confirm every requested fix is present in the edited tree.

A full PlatformIO firmware build was not run in this container because the PlatformIO toolchain is not installed here. The package is therefore a source drop intended to be built/flashed in the normal inkMOD development environment and tested on X3/X4/X4 Pro hardware.
