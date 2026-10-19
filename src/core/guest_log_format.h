/*
	guest_log_format.h — formats a guest printf-style string with up to 6 long args.
	Supported: %lx %lX %ld %lu (with flags/width/precision up to 999), %s (guest C
	string pointer), %S (Pascal string pointer), %% (literal %).
*/
#pragma once
#include <cstdint>
#include <functional>
#include <string>

struct GuestStringReader
{
	std::function<std::string(uint32_t)> cstr; /* C string at a guest address */
	std::function<std::string(uint32_t)> pstr; /* Pascal string at a guest address */
};

/* args[1..6] are the six values consumed by the conversions. */
std::string formatGuestLog(const std::string &fmt, const uint32_t args[7], const GuestStringReader &rd);
