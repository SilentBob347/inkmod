#pragma once

class GfxRenderer;

// Applies the current InkMODSettings::uiTextSize setting by swapping which
// compiled Inter size backs UI_10_FONT_ID (menu/list body text; see
// InkMODSettings::UI_TEXT_SIZE and the comment on applyUiTextSize's
// definition in main.cpp for why SMALL_FONT_ID/UI_12_FONT_ID are left
// alone). The font ID itself never changes, so every existing call site
// (renderer.drawText(UI_10_FONT_ID, ...), etc.) picks up the new size
// automatically with no changes elsewhere.
//
// Call once at boot (after the base fonts are registered) and again
// whenever the user changes the setting, so it takes effect immediately
// without a restart.
void applyUiTextSize(GfxRenderer& renderer);
