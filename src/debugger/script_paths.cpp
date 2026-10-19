/*
	script_paths.cpp — where scripted runs write their output files
*/
#include "debugger/script_paths.h"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

std::string ResolveOutputPath(std::string_view name, std::string_view outDir)
{
	fs::path p{std::string(name)};
	if (!p.has_extension()) p += ".png";
	if (!outDir.empty() && p.is_relative()) p = fs::path(std::string(outDir)) / p;
	return p.string();
}

std::string ScriptOutputPath(const std::string &name)
{
	const char *dir = std::getenv("MAXIVMAC_OUT_DIR");
	std::string out = ResolveOutputPath(name, dir ? dir : "");
	std::error_code ec;
	auto parent = fs::path(out).parent_path();
	if (!parent.empty()) fs::create_directories(parent, ec);
	return out;
}

std::string AbsoluteOutputPath(const std::string &path)
{
	std::error_code ec;
	auto abs = fs::absolute(fs::path(path), ec);
	return ec ? path : abs.lexically_normal().string();
}
