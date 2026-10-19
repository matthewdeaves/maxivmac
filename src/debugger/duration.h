/*
	duration.h — Human-readable duration parser for debugger

	Converts duration strings to cycle counts and back.
*/
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct Token;

// Parse a human-readable duration into a count of scheduler units.
// `clockHz` is the number of scheduler units per second of emulated time
// (for the script commands see ScriptUnitsPerSecond()).
// Accepts:
//   "5s", "500ms"    — emulated time, converted via clockHz
//   "5M", "100k"     — SI multipliers on raw scheduler units
//   "40000000", "5c" — raw scheduler units
//   "off", "none", "0" — returns 0 (meaning infinite/disabled)
// Returns true on success.
bool ParseDuration(std::string_view text, uint64_t clockHz,
				   uint64_t &outCycles);

// Parse a duration from a tokenized command line.  The tokenizer splits
// "20s" into Number(20) + Word(s), so this joins a number with an
// immediately following unit word (s, ms, M, k, K, c).  A bare number is
// taken as raw units, "off"/"none" as 0.  On success `used` is the number
// of tokens consumed (1 or 2).
bool ParseDurationTokens(const std::vector<Token> &toks, size_t idx,
						 uint64_t unitsPerSecond, uint64_t &out, size_t &used);

// Format a cycle count as a human-readable string.
// Uses the most natural unit (e.g., "5s" if evenly divisible).
std::string FormatDuration(uint64_t cycles, uint64_t clockHz);
