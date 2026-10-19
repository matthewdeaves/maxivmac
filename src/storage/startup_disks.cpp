/*
	startup_disks.cpp — see startup_disks.h.
*/

#include "storage/startup_disks.h"

void StartupDisks::set(std::vector<std::string> paths)
{
	paths_.clear();
	for (auto &p : paths)
		if (!p.empty()) paths_.push_back(std::move(p));
}

size_t StartupDisks::restore(const std::function<bool(const std::string &)> &insert) const
{
	size_t n = 0;
	for (const auto &p : paths_)
		if (insert(p)) n++;
	return n;
}
