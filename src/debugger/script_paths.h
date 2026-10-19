/*
	script_paths.h — where scripted runs write their output files
*/
#pragma once

#include <string>
#include <string_view>

// Resolve an output file name used by `screenshot` (and the automatic
// fail/timeout screenshots): a ".png" extension is added when the name has
// none; a relative name is placed under `outDir` when that is non-empty
// (absolute names are left alone).  Pure; see ScriptOutputPath().
std::string ResolveOutputPath(std::string_view name, std::string_view outDir);

// ResolveOutputPath() with outDir taken from $MAXIVMAC_OUT_DIR, creating the
// destination directory when needed.  Without the variable, relative names
// are relative to the current directory.
std::string ScriptOutputPath(const std::string &name);
