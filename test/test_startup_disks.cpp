// StartupDisks: the images re-inserted after a guest reset.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "storage/startup_disks.h"

TEST_CASE("startup disks: restored in the order they were given")
{
	StartupDisks s;
	s.set({"/img/boot.hfs", "/img/data.hfs", "/img/extra.dsk"});
	std::vector<std::string> seen;
	const size_t n = s.restore([&](const std::string &p) { seen.push_back(p); return true; });
	CHECK(n == 3);
	CHECK(seen == std::vector<std::string>{"/img/boot.hfs", "/img/data.hfs", "/img/extra.dsk"});
}

TEST_CASE("startup disks: empty paths are dropped and an empty list restores nothing")
{
	StartupDisks s;
	CHECK(s.restore([](const std::string &) { return true; }) == 0);
	s.set({"", "a.hfs", ""});
	REQUIRE(s.paths().size() == 1);
	CHECK(s.paths()[0] == "a.hfs");
}

TEST_CASE("startup disks: one image that cannot be opened does not stop the rest")
{
	StartupDisks s;
	s.set({"a", "missing", "c"});
	std::vector<std::string> tried;
	const size_t n = s.restore([&](const std::string &p) { tried.push_back(p); return p != "missing"; });
	CHECK(n == 2);
	CHECK(tried.size() == 3);
}

TEST_CASE("startup disks: restoring twice gives the same list (list is not consumed)")
{
	StartupDisks s;
	s.set({"a", "b"});
	int calls = 0;
	s.restore([&](const std::string &) { calls++; return true; });
	s.restore([&](const std::string &) { calls++; return true; });
	CHECK(calls == 4);
}
