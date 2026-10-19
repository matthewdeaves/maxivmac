// Tests for the libslirp timer table (src/devices/slirp_timers.h).
#include <doctest/doctest.h>

#include "devices/slirp_timers.h"

TEST_CASE("slirp timers: only armed, due timers fire, once")
{
	SlirpTimers ts;
	int a = 0, b = 0;
	auto *t1 = ts.add(1, &a);
	auto *t2 = ts.add(2, &b);
	CHECK(ts.armed() == 0);
	SlirpTimers::arm(t1, 100);
	SlirpTimers::arm(t2, 200);
	CHECK(ts.armed() == 2);

	std::vector<int> fired;
	auto fire = [&](int id, void *) { fired.push_back(id); };
	CHECK(ts.fireDue(99, fire) == 0);
	CHECK(ts.fireDue(100, fire) == 1);
	CHECK(fired == std::vector<int>{1});
	CHECK(ts.armed() == 1);
	CHECK(ts.fireDue(100, fire) == 0); // disarmed after firing
	CHECK(ts.fireDue(500, fire) == 1);
	CHECK(fired == std::vector<int>{1, 2});
}

TEST_CASE("slirp timers: a callback may re-arm or free timers")
{
	SlirpTimers ts;
	auto *t1 = ts.add(1, nullptr);
	auto *t2 = ts.add(2, nullptr);
	SlirpTimers::arm(t1, 10);
	SlirpTimers::arm(t2, 10);
	int fired = 0;
	ts.fireDue(10, [&](int id, void *) {
		fired++;
		if (id == 1)
		{
			ts.remove(t2); // freed before its turn: must not fire or crash
			SlirpTimers::arm(t1, 50);
		}
	});
	CHECK(fired == 1);
	CHECK(ts.armed() == 1);
	CHECK(ts.fireDue(50, [&](int, void *) { fired++; }) == 1);
	CHECK(fired == 2);
}
