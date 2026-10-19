/*
	disk_snapshot.cpp — see disk_snapshot.h.
*/
#include "storage/disk_snapshot.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

DiskSnapshots::~DiskSnapshots()
{
	for (auto &kv : copies_) fclose(kv.second);
}

FILE *DiskSnapshots::open(const std::string &path, std::string *err)
{
	auto fail = [&](const char *what) -> FILE * {
		if (err) *err = std::string(what) + ": " + strerror(errno);
		return nullptr;
	};

	auto it = copies_.find(path);
	if (it == copies_.end())
	{
		FILE *src = fopen(path.c_str(), "rb");
		if (!src) return fail("cannot read image");
		FILE *copy = tmpfile();
		if (!copy)
		{
			fclose(src);
			return fail("cannot create temporary file");
		}
		char buf[64 * 1024];
		size_t n;
		bool ok = true;
		while ((n = fread(buf, 1, sizeof buf, src)) > 0)
			if (fwrite(buf, 1, n, copy) != n)
			{
				ok = false;
				break;
			}
		const bool readErr = ferror(src) != 0;
		fclose(src);
		if (!ok || readErr || fflush(copy) != 0)
		{
			const int e = errno;
			fclose(copy);
			errno = e;
			return fail("cannot copy image");
		}
		it = copies_.emplace(path, copy).first;
	}

	const int fd = dup(fileno(it->second));
	if (fd < 0) return fail("dup");
	FILE *f = fdopen(fd, "rb+");
	if (!f)
	{
		const int e = errno;
		close(fd);
		errno = e;
		return fail("fdopen");
	}
	return f;
}
