/*
	text_history.h — Bounded history of the text the guest drew

	`wait text` fires on text drawn after it was armed.  The history keeps the
	most recent strings with the emulated time they were drawn so a script can
	also ask about text drawn shortly before (`wait text "x" recent`), and so a
	failure message can show what the guest did draw.
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

struct TextRecord
{
	uint64_t time = 0; // scheduler units of emulated time when it was drawn
	std::string text;  // UTF-8
	std::string trap;  // the trap that drew it (DrawString, ...)
};

class TextHistory
{
public:
	static constexpr size_t kDefaultCapacity = 256;

	explicit TextHistory(size_t capacity = kDefaultCapacity) : capacity_(capacity ? capacity : 1) {}

	// Append; the oldest record is dropped once the history is full.
	void push(uint64_t time, std::string_view text, std::string_view trap);
	void clear() { records_.clear(); }
	size_t size() const { return records_.size(); }
	size_t capacity() const { return capacity_; }

	// Newest record that contains `pattern` and was drawn at or after `since`
	// (null when there is none).
	const TextRecord *find(std::string_view pattern, uint64_t since) const;

	// The newest `n` records, oldest first.
	std::vector<const TextRecord *> last(size_t n) const;

private:
	size_t capacity_;
	std::deque<TextRecord> records_;
};

// The history of the running emulator (filled by ScriptCaptureText).
TextHistory &ScriptTextHistory();

// The newest `n` records, one per line, oldest first, e.g.
//   "  [   12.345s] DrawString \"Special\"\n"
// `unitsPerSecond` converts the record times to seconds.  The header line says how
// many of the kept records are shown.
std::string FormatTextHistory(const TextHistory &h, size_t n, uint64_t unitsPerSecond);
