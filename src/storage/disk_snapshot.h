/*
	DiskSnapshots — private, throw-away copies of disk images (--disk-snapshot).

	By default the emulator writes to the image it is given, so a second run on
	the same image sees what the first one left behind.  With snapshots on, each
	image is copied once into an anonymous temporary file and the guest only ever
	touches the copy; the original is never written and the copy disappears at
	exit.  The copy is kept per path for the life of the process, so the guest's
	changes survive an eject or a restart within the same run.
*/
#pragma once

#include <cstdio>
#include <map>
#include <string>

class DiskSnapshots
{
public:
	DiskSnapshots() = default;
	DiskSnapshots(const DiskSnapshots &) = delete;
	DiskSnapshots &operator=(const DiskSnapshots &) = delete;
	~DiskSnapshots();

	// A new read/write stream on the snapshot of `path` (created on first use
	// by copying the file).  The caller owns it and closes it with fclose().
	// nullptr, with *err set, when `path` cannot be read or the copy fails.
	FILE *open(const std::string &path, std::string *err);

private:
	std::map<std::string, FILE *> copies_;
};
