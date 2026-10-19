// DiskSnapshots: writes go to a throw-away copy, never to the image.
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "storage/disk_snapshot.h"

namespace
{
namespace fs = std::filesystem;

std::string slurp(const fs::path &p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), {});
}

struct TempImage
{
	fs::path path;
	TempImage()
	{
		path = fs::temp_directory_path() / ("maxivmac_snap_" + std::to_string(::rand()) + ".img");
		std::ofstream(path, std::ios::binary) << "ABCDEFGH";
	}
	~TempImage() { std::error_code ec; fs::remove(path, ec); }
};
} // namespace

TEST_CASE("disk snapshot: writes do not reach the original image")
{
	TempImage img;
	std::string err;
	{
		DiskSnapshots snaps;
		FILE *f = snaps.open(img.path.string(), &err);
		REQUIRE(f != nullptr);
		fseek(f, 2, SEEK_SET);
		fwrite("zz", 1, 2, f);
		fclose(f);
	}
	CHECK(slurp(img.path) == "ABCDEFGH");
}

TEST_CASE("disk snapshot: changes survive eject and re-insert within one run")
{
	TempImage img;
	std::string err;
	DiskSnapshots snaps;
	FILE *f = snaps.open(img.path.string(), &err);
	REQUIRE(f != nullptr);
	fseek(f, 0, SEEK_SET);
	fwrite("xy", 1, 2, f);
	fclose(f);

	FILE *g = snaps.open(img.path.string(), &err);
	REQUIRE(g != nullptr);
	char buf[9] = {};
	fseek(g, 0, SEEK_SET);
	CHECK(fread(buf, 1, 8, g) == 8);
	CHECK(std::string(buf) == "xyCDEFGH");
	fclose(g);
	CHECK(slurp(img.path) == "ABCDEFGH");
}

TEST_CASE("disk snapshot: every run starts from the original image")
{
	TempImage img;
	std::string err;
	for (int run = 0; run < 2; run++)
	{
		DiskSnapshots snaps;
		FILE *f = snaps.open(img.path.string(), &err);
		REQUIRE(f != nullptr);
		char buf[3] = {};
		fseek(f, 0, SEEK_SET);
		CHECK(fread(buf, 1, 2, f) == 2);
		CHECK(std::string(buf) == "AB");
		fseek(f, 0, SEEK_SET);
		fwrite("QQ", 1, 2, f);
		fclose(f);
	}
}

TEST_CASE("disk snapshot: a missing image reports an error")
{
	DiskSnapshots snaps;
	std::string err;
	CHECK(snaps.open("/nonexistent/dir/no.img", &err) == nullptr);
	CHECK_FALSE(err.empty());
}
