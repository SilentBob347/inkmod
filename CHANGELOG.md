# Changelog

## [v1.1.2] - 2026-08-07

### Changed
- The accessibility setting **Increase interface text** now also enlarges search fields, the on-screen keyboard, and keyboard help text, while keeping the reader's book font independent.
- EPUB layout now respects semantic book containers such as chapter title groups and figures, and honours image `max-width` rules. This preserves more of publishers' printed-edition composition, including half-page illustrations.
- EPUBs that use standard `figleft` and `figright` image classes now wrap the following text around drop capitals and side illustrations.
- Drop-cap illustrations now align with the first text line instead of retaining a paragraph-sized gap above it.
- Float reservations now continue past figure captions, so following text no longer draws through right-aligned illustrations.
- EPUB poetry and typographic compositions now preserve per-line inline offsets, including the curved layout of "Mouse's Tale".
- The reader now uses the supplied Bookerly and Roboto SD-card font pack. Bookerly is the default, and the size menu shows every size present in the selected family.
- Rebranded the firmware and web portal as inkMOD, with a new boot, sleep-screen, and web logo.
- EPUB and FB2 readers no longer pre-index the next chapter while you are still reading, avoiding unexpected work when leaving image-heavy books.
- Plain `.zip` files are now identified by their contents when opened, so EPUB and FB2 books no longer need `.epub`, `.fb2`, or `.fb2.zip` in their filename.
- Large EPUB, FB2, and image uploads now enable browser-side optimization by default, keeping image preparation off the reader.
- The on-screen keyboard now stays clear of the side buttons and supports English, Russian, and Ukrainian layouts.
- Moving the reader clock to the bottom no longer reserves an empty row at the top of the page.

### Fixed
- List rows now derive their height from the active interface font, preventing large-text labels from touching neighbouring rows. The current-theme marker is a compact reserved pill instead of covering the selected title.
- FB2 loading now keeps its progress bar moving while the on-demand chapter map is built, and no longer probes every not-yet-created virtual chapter file.
- EPUBs that use `div img { max-width:100% }` now display chapter illustrations at full text width instead of their small source thumbnail size.
- Opening several books before an illustration-heavy FB2 no longer carries the previous reader font cache into the next image decode; FB2 chapters now split after two images to keep decoding within the X4 memory budget.
- EPUB line-level poem offsets now reserve a readable line width instead of pushing text into a narrow column at the screen edge.
- The reader's "By book" page counter now remains selected after waking from sleep.
- JPEG illustrations now remain available at safe memory levels where the previous PNG-sized memory reserve unnecessarily skipped them.
- Very large FB2 files now write their chapter slices directly to the SD-card index instead of allocating a second in-memory chapter list during import, preventing X4 restarts while indexing illustration-heavy books.
- Search results no longer incorrectly label the first result as the selected value.
- Changing the reader clock between top and bottom now repaginates the chapter, preventing text from overlapping the status bar.
- File search now ignores Russian and Ukrainian letter case and starts its keyboard in the interface language.
- Reset Reader Data now clears every `.inkmod` entry except `wifi.json` and `inkmod-settings.json`.
- OTA releases now embed their GitHub tag version into the firmware, so a manually re-published update is not offered repeatedly after installation.
- The search keyboard now reserves an additional gutter around the X3/X4 side-button column and wraps its help text inside that safe area; Lyra's selected-value pill has a visible right inset.
- Large keyboard keys no longer draw their small alternate symbols over the main glyph. Empty subtitle callbacks no longer turn simple system lists into tall two-line rows.

### Removed
- Removed the online font-management screen and downloaded-font size-range setting.

