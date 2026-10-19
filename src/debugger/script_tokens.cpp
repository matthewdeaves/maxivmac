/*
	script_tokens.cpp — token helpers for the `if` and `on` script commands
*/
#include "debugger/script_cond.h"

#include <string>

size_t FindKeyword(const std::vector<Token> &toks, size_t from, std::string_view kw)
{
	for (size_t i = from; i < toks.size(); ++i)
		if (toks[i].isWord(kw) && !toks[i].quoted) return i;
	return std::string::npos;
}

std::vector<Token> SliceTokens(const std::vector<Token> &toks, size_t from, size_t to)
{
	std::vector<Token> out;
	if (to > toks.size()) to = toks.size();
	for (size_t i = from; i < to; ++i)
	{
		if (toks[i].isEnd()) break;
		out.push_back(toks[i]);
	}
	Token end;
	end.kind = Token::Kind::End;
	end.numValue = 0;
	out.push_back(std::move(end));
	return out;
}

bool ReactiveCommandAllowed(std::string_view n)
{
	static constexpr std::string_view kAllowed[] = {
		"key",	 "type",	  "click",	"drag",		   "move",	  "mousedown", "mouseup",
		"clearkeys", "launch", "exittoshell", "shutdown", "screenshot", "log",	"fail",
		"exit",	 "clear",	  "showtext"};
	for (auto a : kAllowed)
		if (a == n) return true;
	return false;
}
