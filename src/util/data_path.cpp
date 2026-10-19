/*
	data_path.cpp — locate files under the project's data/ tree
*/
#include "util/data_path.h"

#include <cstdlib>
#include <filesystem>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(__linux__)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static std::string ExecutableDir()
{
#if defined(__APPLE__)
	char buf[4096];
	uint32_t size = sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) == 0)
	{
		std::error_code ec;
		auto p = fs::weakly_canonical(buf, ec);
		if (!ec) return p.parent_path().string();
	}
#elif defined(__linux__)
	char buf[4096];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n > 0)
	{
		buf[n] = '\0';
		return fs::path(buf).parent_path().string();
	}
#endif
	return {};
}

std::vector<std::string> DataPathCandidates(std::string_view relPath, std::string_view envRoot,
											std::string_view exeDir, std::string_view sourceDir)
{
	std::vector<std::string> out;
	out.emplace_back(relPath);
	if (!envRoot.empty()) out.push_back((fs::path(envRoot) / relPath).string());
	if (!exeDir.empty())
	{
		fs::path e(exeDir);
		out.push_back((e.parent_path().parent_path() / relPath).string()); // bld/macos/exe
		out.push_back((e.parent_path() / relPath).string());
		out.push_back((e / relPath).string());
	}
	if (!sourceDir.empty()) out.push_back((fs::path(sourceDir) / relPath).string());
	return out;
}

std::string ResolveDataPath(const std::string &relPath)
{
#ifdef MAXIVMAC_SOURCE_DIR
	const char *src = MAXIVMAC_SOURCE_DIR;
#else
	const char *src = "";
#endif
	const char *env = std::getenv("MAXIVMAC_ROOT");
	auto cands = DataPathCandidates(relPath, env ? env : "", ExecutableDir(), src);
	std::error_code ec;
	for (auto &c : cands)
		if (fs::is_regular_file(c, ec)) return c;
	return relPath;
}
