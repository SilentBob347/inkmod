#pragma once

// Only the system UI font (Inter) ships with this build. All reading-font
// families (Bitter, ChareInk7, LexendDeca) and their emoji/symbol fallbacks
// have been removed; the interface font is used everywhere, including for
// book text.
//
// Exception: lexenddeca_14_{regular,bold} - reintroduced solely as the
// "Large" tier of UI_10_FONT_ID for themes with roomy enough list rows (see
// applyUiTextSize() in main.cpp). Adds ~566 KB to the firmware; if that's
// too tight against the flash budget, drop the bold half first (used only
// for bold body text in that one mode) before dropping the feature.
#include <builtinFonts/inter_10_bold.h>
#include <builtinFonts/inter_10_regular.h>
#include <builtinFonts/inter_12_bold.h>
#include <builtinFonts/inter_12_regular.h>
#include <builtinFonts/inter_8_regular.h>
#include <builtinFonts/lexenddeca_14_bold.h>
#include <builtinFonts/lexenddeca_14_regular.h>
