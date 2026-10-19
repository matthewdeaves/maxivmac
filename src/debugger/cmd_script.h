// Script commands — public declarations for wait/timeout/fail.
#pragma once

#include <cstdint>
#include <vector>

using ScaledCycleCount = uint64_t;

// Check all scriptOwned breakpoints for timeout expiry.
// Called from the tick handler (60 Hz).
void CheckScriptTimeouts();

// Scheduler units per second of emulated time for the running machine.
// One unit is 1/kCycleScale (1/64) of a 68000 clock cycle, so a Mac Plus
// (7.8336 MHz) runs 501,350,400 units per emulated second.  All raw
// numbers in script commands (wait budgets, `set default-timeout`) are in
// these units; "5s" / "500ms" suffixes are converted with this rate.
uint64_t ScriptUnitsPerSecond();

// `log "message"`: print the message with the emulated time.
struct Token;
void ScriptLogMessage(class Debugger &dbg, const std::vector<Token> &args);

// Default timeout budget for wait commands, in scheduler units
// (setting "default-timeout"; 0 = infinite).
ScaledCycleCount ScriptDefaultTimeout();
