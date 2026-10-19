/*
	test_main — doctest entry point for the unified test binary.

	Fixtures and data files are looked up relative to the source tree, so
	the binary switches to it before running: `tests` behaves the same from
	any directory (and under ctest).
*/
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

#ifndef MAXIVMAC_TEST_SOURCE_DIR
#error "MAXIVMAC_TEST_SOURCE_DIR must be defined by the build"
#endif

int main(int argc, char **argv)
{
	std::error_code ec;
	std::filesystem::current_path(MAXIVMAC_TEST_SOURCE_DIR, ec);
	if (ec) {
		std::fprintf(stderr, "tests: cannot enter %s: %s\n",
			MAXIVMAC_TEST_SOURCE_DIR, ec.message().c_str());
		return 1;
	}
	doctest::Context ctx(argc, argv);
	return ctx.run();
}
