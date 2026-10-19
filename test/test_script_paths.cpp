/*
	test_script_paths — output file naming for scripted runs
*/

#include <doctest/doctest.h>
#include "debugger/script_paths.h"

TEST_CASE("output path: .png is added when there is no extension")
{
	CHECK(ResolveOutputPath("desktop", "") == "desktop.png");
	CHECK(ResolveOutputPath("shot.png", "") == "shot.png");
	CHECK(ResolveOutputPath("a.b/shot", "") == "a.b/shot.png");
}

TEST_CASE("output path: relative names go under the output directory")
{
	CHECK(ResolveOutputPath("desktop", "/out") == "/out/desktop.png");
	CHECK(ResolveOutputPath("sub/menu", "out") == "out/sub/menu.png");
}

TEST_CASE("output path: absolute names are left alone")
{
	CHECK(ResolveOutputPath("/tmp/x", "/out") == "/tmp/x.png");
}

TEST_CASE("output path: absolute form for messages")
{
	CHECK(AbsoluteOutputPath("/tmp/x.png") == "/tmp/x.png");
	auto rel = AbsoluteOutputPath("sub/./shot.png");
	CHECK(rel.front() == '/');
	CHECK(rel.size() > std::string("/sub/shot.png").size());
	CHECK(rel.substr(rel.size() - 12) == "sub/shot.png");
}
