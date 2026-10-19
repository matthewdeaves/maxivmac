/*
	test_script_keys — timing of scripted key presses (`key ... hold`)
*/

#include <doctest/doctest.h>
#include "debugger/script_keymap.h"
#include "platform/keycodes.h"

TEST_CASE("key plan: a plain key keeps the legacy timing at the default hold")
{
	auto p = PlanKeyPress(0, MKC_Return, kKeyDefaultHold);
	REQUIRE(p.size() == 2);
	CHECK(p[0].key == MKC_Return);
	CHECK(p[0].down);
	CHECK(p[0].dt == 10000);
	CHECK_FALSE(p[1].down);
	CHECK(p[1].dt == 90000);
}

TEST_CASE("key plan: modifiers bracket the key and are released in reverse order")
{
	auto p = PlanKeyPress(kModCmd | kModShift, MKC_A, 1000000);
	REQUIRE(p.size() == 6);
	CHECK(p[0].key == MKC_Command);
	CHECK(p[0].down);
	CHECK(p[0].dt == 0);
	CHECK(p[1].key == MKC_Shift);
	CHECK(p[2].key == MKC_A);
	CHECK(p[2].down);
	CHECK(p[2].dt == kKeyModLead);
	CHECK(p[3].key == MKC_A);
	CHECK_FALSE(p[3].down);
	CHECK(p[3].dt == kKeyModLead + 1000000); // the key is down for exactly `hold`
	CHECK(p[4].key == MKC_Shift);
	CHECK_FALSE(p[4].down);
	CHECK(p[4].dt == p[3].dt + kKeyModTail);
	CHECK(p[5].key == MKC_Command);
	CHECK_FALSE(p[5].down);
}

TEST_CASE("key plan: edges are in time order whatever the hold")
{
	for (uint64_t hold : {uint64_t(0), uint64_t(1), uint64_t(80000), uint64_t(500000000)})
	{
		auto p = PlanKeyPress(kModCmd | kModShift | kModOption | kModCtrl, MKC_K, hold);
		for (size_t i = 1; i < p.size(); ++i) CHECK(p[i].dt >= p[i - 1].dt);
	}
}

TEST_CASE("key names: modifiers can be pressed on their own")
{
	CHECK(ParseKeySpec("shift").keycode == MKC_Shift);
	CHECK(ParseKeySpec("cmd").keycode == MKC_Command);
	CHECK(ParseKeySpec("opt").keycode == MKC_Option);
	CHECK(ParseKeySpec("cmd-shift").keycode == MKC_Shift);
	CHECK(ParseKeySpec("cmd-shift").modifiers == kModCmd);
	CHECK(ParseKeySpec("nosuchkey").keycode == 0xFF);
}
