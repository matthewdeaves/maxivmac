/*
	script_cond.cpp — `if` and `on` script commands
*/
#include "debugger/script_cond.h"
#include "debugger/debugger.h"
#include "debugger/dbg_io.h"
#include "debugger/cmd_script.h"
#include "debugger/duration.h"
#include "debugger/text_history.h"

#include "core/ict_scheduler.h"

#include <cstdlib>
#include <string>

static void Bad(Debugger &dbg, const char *usage)
{
	dbg.io().write("%s\n", usage);
	if (dbg.scriptMode())
	{
		dbg.io().write("FAIL: bad script command\n");
		std::exit(2);
	}
}

static constexpr const char *kIfUsage =
	"Usage: if text \"pattern\" [recent [window]] then <command> [else <command>]";
static constexpr const char *kOnUsage =
	"Usage: on text \"pattern\" [once] do <command>\n"
	"       on list\n"
	"       on clear";

/* Run a command given as tokens (first token = command name). */
static bool RunTokens(Debugger &dbg, const std::vector<Token> &toks)
{
	if (toks.empty() || toks[0].isEnd()) return true;
	auto *entry = DispatchCommand(toks[0].text, dbg.commandTable(), dbg.commandTableSize(), &dbg.io());
	if (!entry)
	{
		if (dbg.scriptMode())
		{
			dbg.io().write("FAIL: cannot run script command: %s\n", toks[0].text.c_str());
			std::exit(2);
		}
		return false;
	}
	std::vector<Token> args(toks.begin() + 1, toks.end());
	entry->handler(dbg, args);
	return true;
}

/* ── if ─────────────────────────────────────────────── */

void CmdIf(Debugger &dbg, const std::vector<Token> &args)
{
	if (args.size() < 2 || !args[0].isWord("text") || args[1].isEnd())
	{
		Bad(dbg, kIfUsage);
		return;
	}
	const std::string &pattern = args[1].text;
	const uint64_t hz = ScriptUnitsPerSecond();
	size_t idx = 2;
	uint64_t window = 5 * hz;
	if (idx < args.size() && args[idx].isWord("recent") && !args[idx].quoted)
	{
		++idx;
		size_t used;
		if (idx < args.size() && !args[idx].isWord("then") && !args[idx].isEnd())
		{
			if (!ParseDurationTokens(args, idx, hz, window, used))
			{
				Bad(dbg, "Error: cannot parse the window after 'recent' (use 5s, 500ms)");
				return;
			}
			idx += used;
		}
	}
	if (idx >= args.size() || !args[idx].isWord("then") || args[idx].quoted)
	{
		Bad(dbg, kIfUsage);
		return;
	}
	size_t elsePos = FindKeyword(args, idx + 1, "else");
	size_t thenEnd = elsePos == std::string::npos ? args.size() : elsePos;
	auto thenCmd = SliceTokens(args, idx + 1, thenEnd);
	auto elseCmd = elsePos == std::string::npos ? std::vector<Token>()
												 : SliceTokens(args, elsePos + 1, args.size());
	if (thenCmd.size() < 2 || (elsePos != std::string::npos && elseCmd.size() < 2))
	{
		Bad(dbg, kIfUsage);
		return;
	}

	ScaledCycleCount now = g_ict.getCurrent();
	uint64_t since = (window == 0 || window > now) ? 0 : now - window;
	const TextRecord *rec = ScriptTextHistory().find(pattern, since);
	dbg.io().write("if text \"%s\": %s\n", pattern.c_str(), rec ? "yes" : "no");
	RunTokens(dbg, rec ? thenCmd : elseCmd);
}

/* ── on ─────────────────────────────────────────────── */

namespace
{
struct Handler
{
	uint32_t id = 0;
	std::string pattern;
	bool once = false;
	std::vector<Token> command;
	std::string origin; // where `on` was written
};
std::vector<Handler> s_handlers;
uint32_t s_nextHandlerId = 1;
bool s_inHandler = false;
} // namespace

void CmdOn(Debugger &dbg, const std::vector<Token> &args)
{
	if (!args.empty() && args[0].isWord("list"))
	{
		if (s_handlers.empty()) dbg.io().write("No text handlers\n");
		for (auto &h : s_handlers)
			dbg.io().write("  %u: on text \"%s\"%s do %s   (%s)\n", h.id, h.pattern.c_str(),
						   h.once ? " once" : "", h.command[0].text.c_str(), h.origin.c_str());
		return;
	}
	if (!args.empty() && args[0].isWord("clear"))
	{
		s_handlers.clear();
		dbg.io().write("Cleared the text handlers\n");
		return;
	}
	if (args.size() < 2 || !args[0].isWord("text") || args[1].isEnd())
	{
		Bad(dbg, kOnUsage);
		return;
	}
	size_t idx = 2;
	Handler h;
	h.pattern = args[1].text;
	if (idx < args.size() && args[idx].isWord("once") && !args[idx].quoted)
	{
		h.once = true;
		++idx;
	}
	if (idx >= args.size() || !args[idx].isWord("do") || args[idx].quoted)
	{
		Bad(dbg, kOnUsage);
		return;
	}
	h.command = SliceTokens(args, idx + 1, args.size());
	if (h.command.size() < 2)
	{
		Bad(dbg, kOnUsage);
		return;
	}
	auto *entry = DispatchCommand(h.command[0].text, dbg.commandTable(), dbg.commandTableSize(), &dbg.io());
	if (!entry)
	{
		if (dbg.scriptMode()) std::exit(2);
		return;
	}
	if (!ReactiveCommandAllowed(entry->name))
	{
		dbg.io().write("Error: '%.*s' cannot run from a handler (it would resume the guest); "
					   "use key, type, click, drag, move, mousedown, mouseup, screenshot, log, "
					   "fail, exit, ...\n",
					   static_cast<int>(entry->name.size()), entry->name.data());
		if (dbg.scriptMode()) std::exit(2);
		return;
	}
	h.id = s_nextHandlerId++;
	if (const ScriptLine *l = dbg.currentScriptLine())
		h.origin = l->file.empty() ? "typed" : l->file + ":" + std::to_string(l->line);
	else
		h.origin = "typed";
	dbg.io().write("Handler %u: on text \"%s\"%s do %s\n", h.id, h.pattern.c_str(),
				   h.once ? " once" : "", h.command[0].text.c_str());
	s_handlers.push_back(std::move(h));
}

void ScriptRunTextHandlers(std::string_view utf8Text)
{
	if (s_handlers.empty() || s_inHandler) return;
	auto *dbg = Debugger::instance();
	if (!dbg) return;
	s_inHandler = true;
	// Index loop on a copy of the matches: a handler may clear the list.
	std::vector<Handler> fire;
	for (size_t i = 0; i < s_handlers.size();)
	{
		if (utf8Text.find(s_handlers[i].pattern) == std::string_view::npos)
		{
			++i;
			continue;
		}
		fire.push_back(s_handlers[i]);
		if (s_handlers[i].once)
			s_handlers.erase(s_handlers.begin() + static_cast<std::ptrdiff_t>(i));
		else
			++i;
	}
	for (auto &h : fire)
	{
		dbg->io().write("Handler %u fired: text \"%s\" drawn as \"%.*s\"\n", h.id, h.pattern.c_str(),
						static_cast<int>(utf8Text.size()), utf8Text.data());
		ScriptSetOriginOverride("on text \"" + h.pattern + "\" (" + h.origin + ")");
		RunTokens(*dbg, h.command);
		ScriptSetOriginOverride(std::string());
	}
	s_inHandler = false;
}
