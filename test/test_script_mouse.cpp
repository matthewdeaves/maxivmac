/*
	test_script_mouse — timing plans for scripted mouse gestures
*/

#include <doctest/doctest.h>
#include "debugger/script_mouse.h"

static constexpr ScaledCycleCount kTick = 1000;

/* The device handles one event per tick and ignores input for
   kMouseButtonLockTicks after a button event; plans must respect that. */
static void CheckDeviceSpacing(const std::vector<MouseStep> &plan)
{
	for (size_t i = 1; i < plan.size(); ++i)
	{
		CHECK(plan[i].dt > plan[i - 1].dt);
		CHECK(plan[i].dt - plan[i - 1].dt >= kTick);
		if (plan[i - 1].kind == EvtQElKind::MouseButton)
			CHECK(plan[i].dt - plan[i - 1].dt > kMouseButtonLockTicks * kTick);
	}
}

TEST_CASE("mouse plan: click presses after the pointer arrives")
{
	auto plan = PlanClick(100, 200, 10 * kTick, kTick);
	REQUIRE(plan.size() == 3);
	CHECK(plan[0].kind == EvtQElKind::MousePos);
	CHECK(plan[0].h == 100);
	CHECK(plan[0].v == 200);
	CHECK(plan[0].dt == 0);
	CHECK(plan[1].kind == EvtQElKind::MouseButton);
	CHECK(plan[1].down);
	CHECK(plan[1].dt >= kTick);
	CHECK_FALSE(plan[2].down);
	CHECK(plan[2].dt - plan[1].dt == 10 * kTick);
	CheckDeviceSpacing(plan);
}

TEST_CASE("mouse plan: click hold is clamped above the device lock")
{
	auto plan = PlanClick(1, 2, 0, kTick);
	REQUIRE(plan.size() == 3);
	CHECK(plan[2].dt - plan[1].dt > kMouseButtonLockTicks * kTick);
	CheckDeviceSpacing(plan);
}

TEST_CASE("mouse plan: drag glides from start to end while pressed")
{
	auto plan = PlanDrag(10, 20, 110, 220, 5 * kTick, 20 * kTick, kTick, 10);
	REQUIRE(plan.size() == 13); // move, down, 10 steps, up
	CHECK(plan.front().kind == EvtQElKind::MousePos);
	CHECK(plan.front().h == 10);
	CHECK(plan[1].kind == EvtQElKind::MouseButton);
	CHECK(plan[1].down);
	CHECK(plan[11].kind == EvtQElKind::MousePos);
	CHECK(plan[11].h == 110);
	CHECK(plan[11].v == 220);
	CHECK_FALSE(plan.back().down);
	CHECK(plan.back().kind == EvtQElKind::MouseButton);
	/* monotone glide */
	for (size_t i = 3; i < 12; ++i)
	{
		CHECK(plan[i].h > plan[i - 1].h);
		CHECK(plan[i].v > plan[i - 1].v);
	}
	/* dwell at the destination before release */
	CHECK(plan.back().dt - plan[11].dt == 20 * kTick);
	CheckDeviceSpacing(plan);
}

TEST_CASE("mouse plan: drag enforces minimum step and settle times")
{
	auto plan = PlanDrag(0, 0, 50, 0, 1, 1, kTick, 4);
	CheckDeviceSpacing(plan);
	CHECK(plan.back().dt - plan[plan.size() - 2].dt >= 8 * kTick);
	/* moves while pressed are at least 3 ticks apart */
	for (size_t i = 3; i + 1 < plan.size(); ++i)
		CHECK(plan[i].dt - plan[i - 1].dt >= 3 * kTick);
}

TEST_CASE("mouse plan: drag handles right-to-left and upward motion")
{
	auto plan = PlanDrag(200, 300, 100, 100, kTick, kTick, kTick, 5);
	REQUIRE(plan.size() == 8); // move, down, 5 steps, up
	CHECK(plan[6].h == 100);
	CHECK(plan[6].v == 100);
	CHECK(plan[3].h < plan[2].h);
}

TEST_CASE("mouse plan: single move and button")
{
	auto m = PlanMove(5, 6);
	REQUIRE(m.size() == 1);
	CHECK(m[0].kind == EvtQElKind::MousePos);
	CHECK(m[0].dt == 0);

	auto d = PlanButton(true, kTick);
	REQUIRE(d.size() == 1);
	CHECK(d[0].down);
	CHECK(d[0].dt >= kTick);
	auto u = PlanButton(false, kTick);
	CHECK_FALSE(u[0].down);
}

TEST_CASE("mouse plan: duration covers the device lock after a button")
{
	CHECK(PlanDuration({}, kTick) == 0);
	auto plan = PlanClick(0, 0, 0, kTick);
	CHECK(PlanDuration(plan, kTick) == plan.back().dt + (kMouseButtonLockTicks + 1) * kTick);
	auto mv = PlanMove(1, 1);
	CHECK(PlanDuration(mv, kTick) == kTick);
}
