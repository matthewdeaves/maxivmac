/*
	script_suspend.h — Script suspension

	Holds remaining lines when a script resumes the CPU.
*/
#pragma once

#include <string>
#include <string_view>
#include <vector>

// One script line with where it came from (for failure messages).
struct ScriptLine
{
	std::string text;
	std::string file; // empty for lines that do not come from a file
	int line = 0;	  // 1-based line number in `file`
};

// Holds the remaining lines of a suspended script.
// When a script command resumes the CPU (e.g. wait, continue),
// execution suspends here and resumes when the debugger next stops.
struct PendingScript
{
	std::vector<ScriptLine> lines;
	int nextLine = 0;
	std::string sourceFile; // for error messages

	bool exhausted() const { return nextLine >= static_cast<int>(lines.size()); }
	std::string_view currentLine() const { return lines[nextLine].text; }
	void advance() { ++nextLine; }
	void clear()
	{
		lines.clear();
		nextLine = 0;
		sourceFile.clear();
	}
};
