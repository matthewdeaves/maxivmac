/*
	script_mouse.h — timing plans for scripted mouse gestures

	The emulated mouse hardware is sampled once per 60 Hz tick: at most one
	queued mouse event is consumed per tick, and after a button event the
	device ignores further mouse events for 4 ticks (see MouseDevice::update).
	A press/release pair injected closer together than that is seen by the
	guest as an instantaneous blip, which is too fast for the Menu Manager or
	for dragging.  These planners therefore space events in whole ticks.

	Plans are pure data (offsets from the start of the gesture) so they can be
	unit tested; cmd_script.cpp turns them into EventQ events.
*/
#pragma once

#include <cstdint>
#include <vector>

#include "core/ict_scheduler.h" // ScaledCycleCount
#include "platform/platform.h"	// EvtQElKind

struct MouseStep
{
	ScaledCycleCount dt;  // offset from the start of the gesture
	EvtQElKind kind;	  // MousePos or MouseButton
	int16_t h = 0, v = 0; // MousePos
	bool down = false;	  // MouseButton
};

// Minimum gap after a button event before the device accepts another event.
inline constexpr int kMouseButtonLockTicks = 4;

// Move to (h,v), then press and release after `holdUnits` (clamped to at
// least kMouseButtonLockTicks + 1 ticks so the guest sees the press).
std::vector<MouseStep> PlanClick(int16_t h, int16_t v, ScaledCycleCount holdUnits,
								 ScaledCycleCount unitsPerTick);

// Move to (h1,v1), press, glide to (h2,v2) in `steps` evenly spaced moves
// (`stepUnits` apart, at least 3 ticks), dwell `settleUnits` at the end
// (at least 8 ticks, enough for menu highlighting), then release.
std::vector<MouseStep> PlanDrag(int16_t h1, int16_t v1, int16_t h2, int16_t v2,
								ScaledCycleCount stepUnits, ScaledCycleCount settleUnits,
								ScaledCycleCount unitsPerTick, int steps = 10);

// Single pointer move.
std::vector<MouseStep> PlanMove(int16_t h, int16_t v);

// Single button press (down = true) or release.  Preceded by a short lead
// so a pointer move queued just before is delivered first.
std::vector<MouseStep> PlanButton(bool down, ScaledCycleCount unitsPerTick);

// Time from the start of a plan until the device will accept the next
// gesture (last event + lock for button events, + 1 tick otherwise).
ScaledCycleCount PlanDuration(const std::vector<MouseStep> &plan, ScaledCycleCount unitsPerTick);
