/*
	data_path.h — locate files under the project's data/ tree

	The emulator ships definition files (data/debug/traps.def, ...) that must
	be found no matter which directory it is started from.  A path such as
	"data/debug/traps.def" is looked up in this order:

	  1. relative to the current directory (historic behaviour)
	  2. $MAXIVMAC_ROOT/<path>
	  3. next to / above the executable (<exe>/../.. , <exe>/.. , <exe>)
	  4. the source tree this binary was built from (MAXIVMAC_SOURCE_DIR)
*/
#pragma once

#include <string>
#include <string_view>
#include <vector>

// Ordered list of candidate locations for `relPath` (see above).
// `exeDir` may be empty.  Pure function, exposed for testing.
std::vector<std::string> DataPathCandidates(std::string_view relPath, std::string_view envRoot,
											std::string_view exeDir, std::string_view sourceDir);

// Resolve `relPath` to the first candidate that exists as a regular file.
// Returns `relPath` unchanged when nothing matches, so callers keep their
// existing "file not found" handling.
std::string ResolveDataPath(const std::string &relPath);
