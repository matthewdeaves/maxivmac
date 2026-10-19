/*
	script_mouse.cpp — timing plans for scripted mouse gestures
*/
#include "debugger/script_mouse.h"

#include <algorithm>

static MouseStep Pos(ScaledCycleCount dt, int16_t h, int16_t v)
{
	MouseStep s{};
	s.dt = dt;
	s.kind = EvtQElKind::MousePos;
	s.h = h;
	s.v = v;
	return s;
}

static MouseStep Btn(ScaledCycleCount dt, bool down)
{
	MouseStep s{};
	s.dt = dt;
	s.kind = EvtQElKind::MouseButton;
	s.down = down;
	return s;
}

std::vector<MouseStep> PlanClick(int16_t h, int16_t v, ScaledCycleCount holdUnits,
								 ScaledCycleCount tick)
{
	ScaledCycleCount minHold = (kMouseButtonLockTicks + 1) * tick;
	ScaledCycleCount hold = std::max(holdUnits, minHold);
	ScaledCycleCount down = 2 * tick; // let the pointer position land first
	return {Pos(0, h, v), Btn(down, true), Btn(down + hold, false)};
}

std::vector<MouseStep> PlanDrag(int16_t h1, int16_t v1, int16_t h2, int16_t v2,
								ScaledCycleCount stepUnits, ScaledCycleCount settleUnits,
								ScaledCycleCount tick, int steps)
{
	if (steps < 1) steps = 1;
	ScaledCycleCount step = std::max(stepUnits, 3 * tick);
	ScaledCycleCount settle = std::max(settleUnits, 8 * tick);

	std::vector<MouseStep> plan;
	plan.push_back(Pos(0, h1, v1));
	ScaledCycleCount t = 2 * tick;
	plan.push_back(Btn(t, true));
	t += (kMouseButtonLockTicks + 2) * tick; // device ignores input right after a button event
	for (int i = 1; i <= steps; ++i)
	{
		int h = h1 + (h2 - h1) * i / steps;
		int v = v1 + (v2 - v1) * i / steps;
		plan.push_back(Pos(t, static_cast<int16_t>(h), static_cast<int16_t>(v)));
		if (i < steps) t += step;
	}
	t += settle;
	plan.push_back(Btn(t, false));
	return plan;
}

std::vector<MouseStep> PlanMove(int16_t h, int16_t v)
{
	return {Pos(0, h, v)};
}

std::vector<MouseStep> PlanButton(bool down, ScaledCycleCount tick)
{
	return {Btn(2 * tick, down)};
}

ScaledCycleCount PlanDuration(const std::vector<MouseStep> &plan, ScaledCycleCount tick)
{
	if (plan.empty()) return 0;
	const MouseStep &last = plan.back();
	int extra = last.kind == EvtQElKind::MouseButton ? kMouseButtonLockTicks + 1 : 1;
	return last.dt + extra * tick;
}
