/*
	script_cond.h — `if` and `on`: conditional and reactive script commands

	  if text "pat" [recent [window]] then <command> [else <command>]
	  on text "pat" [once] do <command>
	  on list | on clear

	`if` looks at the history of drawn text now; `on` registers a handler that
	runs when the guest draws matching text, while the script is waiting or
	between lines.  Commands run by a handler must not resume the CPU.
*/
#pragma once

#include "debugger/cmd_parser.h"

#include <cstddef>
#include <string_view>
#include <vector>

// Index in `toks` (from `from`) of the first unquoted word `kw`, or npos.
size_t FindKeyword(const std::vector<Token> &toks, size_t from, std::string_view kw);

// Copy toks[from, to) and terminate it with an End token, so the result looks
// like a tokenized command line (first token = command name).
std::vector<Token> SliceTokens(const std::vector<Token> &toks, size_t from, size_t to);

// Commands a reactive handler may run: they only queue input or print, and do
// not resume the CPU or wait.
bool ReactiveCommandAllowed(std::string_view commandName);

// Fire the handlers registered with `on` for freshly drawn text.
void ScriptRunTextHandlers(std::string_view utf8Text);
