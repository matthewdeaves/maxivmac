#include "debugger/text_history.h"

#include <cstdio>

void TextHistory::push(uint64_t time, std::string_view text, std::string_view trap)
{
	if (records_.size() >= capacity_) records_.pop_front();
	records_.push_back({time, std::string(text), std::string(trap)});
}

const TextRecord *TextHistory::find(std::string_view pattern, uint64_t since) const
{
	for (auto it = records_.rbegin(); it != records_.rend(); ++it)
	{
		if (it->time < since) break; // older records are older still
		if (it->text.find(pattern) != std::string::npos) return &*it;
	}
	return nullptr;
}

std::vector<const TextRecord *> TextHistory::last(size_t n) const
{
	std::vector<const TextRecord *> out;
	size_t skip = records_.size() > n ? records_.size() - n : 0;
	for (size_t i = skip; i < records_.size(); ++i) out.push_back(&records_[i]);
	return out;
}

std::string FormatTextHistory(const TextHistory &h, size_t n, uint64_t unitsPerSecond)
{
	auto recs = h.last(n);
	std::string out;
	char buf[64];
	if (recs.empty()) return "  recent text: none drawn\n";
	std::snprintf(buf, sizeof buf, "  recent text (newest last, %zu of %zu):\n", recs.size(),
				  h.size());
	out += buf;
	for (auto *r : recs)
	{
		std::snprintf(buf, sizeof buf, "    [%9.3fs] ",
					  static_cast<double>(r->time) / static_cast<double>(unitsPerSecond));
		out += buf;
		out += r->trap;
		out += " \"";
		for (char c : r->text) // one line per string, whatever the guest drew
		{
			if (c == '\n')
				out += "\\n";
			else if (c == '\r')
				out += "\\r";
			else if (c == '\t')
				out += "\\t";
			else
				out += c;
		}
		out += "\"\n";
	}
	return out;
}
