#include "debugger/text_history.h"

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
