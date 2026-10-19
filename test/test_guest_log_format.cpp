/*
	test_guest_log_format — guest printf formatting for the clip/extfs log calls
*/
#include <doctest/doctest.h>
#include "core/guest_log_format.h"

static std::string fmt(const char *f, uint32_t a1 = 0, uint32_t a2 = 0)
{
	const uint32_t args[7] = {0, a1, a2, 0, 0, 0, 0};
	GuestStringReader rd{[](uint32_t a) { return "s" + std::to_string(a); }, [](uint32_t a) { return "p" + std::to_string(a); }};
	return formatGuestLog(f, args, rd);
}

TEST_CASE("guest log: zero-padded widths keep every digit")
{
	CHECK(fmt("%010x", 0x12345678) == "0012345678");
	CHECK(fmt("%08lx", 0xBEEF) == "0000beef");
	CHECK(fmt("%02x", 5) == "05");
	CHECK(fmt("%x", 0x1f) == "1f");
	CHECK(fmt("%-4d|", 7) == "7   |");
	CHECK(fmt("%X", 0xabc) == "ABC");
}

TEST_CASE("guest log: strings, literals and missing arguments")
{
	CHECK(fmt("a%%b %s %S", 3, 4) == "a%b s3 p4");
	CHECK(fmt("%q") == "%q");
}

TEST_CASE("guest log: absurd widths are capped, not honoured")
{
	/* must return promptly and never exceed the formatting buffer */
	CHECK(fmt("%.2000000000x", 1).size() < 32);
	CHECK(fmt("%99999999x", 1).size() < 32);
}
