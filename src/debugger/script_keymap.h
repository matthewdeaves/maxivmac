/*
	script_keymap.h — Script keymap

	Maps characters and key specs to Mac virtual keycodes.
*/
#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

// Map a MacRoman byte to (Mac virtual keycode, needShift).
// Returns {0xFF, false} for unmappable characters.
std::pair<uint8_t, bool> CharToMacKey(uint8_t macRomanChar);

// Parse a modifier+key spec like "cmd-shift-S" or "return".
// Returns (keycode, modifierMask). modifierMask bits:
//   bit 0 = cmd, bit 1 = shift, bit 2 = option, bit 3 = ctrl
struct KeySpec
{
	uint8_t keycode;
	uint8_t modifiers;
};
KeySpec ParseKeySpec(std::string_view spec);

// Modifier mask bits
inline constexpr uint8_t kModCmd = 0x01;
inline constexpr uint8_t kModShift = 0x02;
inline constexpr uint8_t kModOption = 0x04;
inline constexpr uint8_t kModCtrl = 0x08;

// One edge of a key press, `dt` scheduler units after the press began.
struct KeyEdge
{
	uint64_t dt;
	uint8_t key; // Mac virtual keycode (modifiers included)
	bool down;
};

// Margins around the key itself, and the hold of the key without `hold`
// (scheduler units; `key` raises the default to at least one tick).
inline constexpr uint64_t kKeyModLead = 10000;	// modifiers go down this long before the key
inline constexpr uint64_t kKeyModTail = 10000;	// ... and up this long after it is released
inline constexpr uint64_t kKeyDefaultHold = 80000;

// The edges of pressing `keycode` with `modifiers` (kMod* bits), holding it for
// `hold`: modifiers down, key down kKeyModLead later, key up `hold` after
// that, modifiers up kKeyModTail after that.  Sorted by time.
std::vector<KeyEdge> PlanKeyPress(uint8_t modifiers, uint8_t keycode, uint64_t hold);
