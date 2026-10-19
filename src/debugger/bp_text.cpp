/*
	bp_text.cpp — Text breakpoint matching implementation

	Fires when trap parameters contain target text.
*/
#include "debugger/bp_text.h"
#include "debugger/debugger.h"
#include "debugger/dbg_io.h"
#include "debugger/text_history.h"
#include "debugger/script_cond.h"
#include "core/machine.h"
#include "core/ict_scheduler.h"

TextHistory &ScriptTextHistory()
{
	static TextHistory history;
	return history;
}

static bool s_showText = false;

void ScriptShowTextSet(bool on)
{
	s_showText = on;
}
bool ScriptShowTextGet()
{
	return s_showText;
}

void ScriptCaptureText(std::string_view utf8Text, std::string_view trapName)
{
	auto *dbg = Debugger::instance();
	if (!dbg) return;

	// Remember it, so `wait text ... recent` and failure messages can see it
	ScriptTextHistory().push(g_ict.getCurrent(), utf8Text, trapName);

	// Show captured text if enabled
	if (s_showText)
		dbg->io().write("[text] %.*s: \"%.*s\"\n", static_cast<int>(trapName.size()),
						trapName.data(), static_cast<int>(utf8Text.size()), utf8Text.data());

	// Reactive handlers (`on text ... do ...`)
	ScriptRunTextHandlers(utf8Text);

	// Check all text breakpoints
	auto &bps = dbg->breakpoints();
	for (size_t i = 0; i < bps.size(); ++i)
	{
		auto &bp = bps[i];
		if (bp.kind != Debugger::Breakpoint::Kind::Text) continue;
		if (!bp.enabled) continue;

		// (Timeouts are enforced by CheckScriptTimeouts(), once per tick.)

		// Substring match
		if (utf8Text.find(bp.textPattern) == std::string_view::npos) continue;

		// Ignore count
		if (bp.ignoreCount > 0)
		{
			--bp.ignoreCount;
			continue;
		}

		// Match! Fire the breakpoint.
		dbg->io().write("Breakpoint %u: text match \"%s\" in %.*s\n", bp.id, bp.textPattern.c_str(),
						static_cast<int>(trapName.size()), trapName.data());

		bool wasTemporary = bp.temporary;
		bool wasScriptOwned = bp.scriptOwned;
		uint32_t bpId = bp.id;

		if (!bp.commands.empty()) dbg->executeCommands(bp.commands);

		if (wasTemporary) dbg->deleteById(bpId);

		dbg->stop("");

		// Try resuming a pending script if scriptOwned
		if (wasScriptOwned) dbg->tryResumeScript(nullptr);

		return; // only fire one text BP per capture
	}
}
