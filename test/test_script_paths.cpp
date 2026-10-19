/*
	test_script_paths — output file naming for scripted runs
*/

#include <doctest/doctest.h>
#include "debugger/script_paths.h"
#include <filesystem>

/* compare with / separators so the checks hold on Windows too */
static std::string G(const std::string &x) { return std::filesystem::path(x).generic_string(); }

TEST_CASE("output path: .png is added when there is no extension")
{
	CHECK(ResolveOutputPath("desktop", "") == "desktop.png");
	CHECK(ResolveOutputPath("shot.png", "") == "shot.png");
	CHECK(G(ResolveOutputPath("a.b/shot", "")) == "a.b/shot.png");
}

TEST_CASE("output path: relative names go under the output directory")
{
	CHECK(G(ResolveOutputPath("desktop", "/out")) == "/out/desktop.png");
	CHECK(G(ResolveOutputPath("sub/menu", "out")) == "out/sub/menu.png");
}

#ifndef _WIN32 /* "/tmp/x" has no drive, so it is not absolute on Windows */
TEST_CASE("output path: absolute names are left alone")
{
	CHECK(ResolveOutputPath("/tmp/x", "/out") == "/tmp/x.png");
}
#endif

TEST_CASE("output path: absolute form for messages")
{
#ifndef _WIN32
	CHECK(AbsoluteOutputPath("/tmp/x.png") == "/tmp/x.png");
#endif
	auto rel = AbsoluteOutputPath("sub/./shot.png");
	CHECK(std::filesystem::path(rel).is_absolute());
	CHECK(rel.size() > std::string("/sub/shot.png").size());
	CHECK(G(rel.substr(rel.size() - 12)) == "sub/shot.png");
}
