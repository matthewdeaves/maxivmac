/*
	StartupDisks — the disk images the machine was started with.

	A guest-initiated reset (Special > Restart, Shut Down -> restart) ejects
	every disk image (see DoMacReset in core/main.cpp), and the ROM then boots
	with nothing to boot from.  A real Mac keeps its disks across a restart, so
	the emulator re-inserts the images it started with, in the same drive
	order.  Images the user added later (drag and drop, Open dialog) are not
	part of the list and stay ejected by a reset, as before.
*/
#pragma once

#include <functional>
#include <string>
#include <vector>

class StartupDisks
{
public:
	// Replace the list; empty paths are ignored.
	void set(std::vector<std::string> paths);
	const std::vector<std::string> &paths() const { return paths_; }

	// Insert every listed image, in order, through `insert` (which returns
	// false when it cannot open the image).  A failing image is skipped so one
	// missing file does not keep the others from coming back.  Returns the
	// number inserted.
	size_t restore(const std::function<bool(const std::string &)> &insert) const;

private:
	std::vector<std::string> paths_;
};
