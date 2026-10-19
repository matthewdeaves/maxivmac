/*
	scsi_factory — create SCSI targets from --scsiN=TYPE strings.
*/
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "devices/scsi_target.h"

struct ScsiTargetOptions
{
	std::string eth;				  // --eth spec ("" = default, see below)
	std::vector<std::string> redirs;  // --net-redir specs
	std::string ethMac;				  // --eth-mac ("" = default MAC)
	// --eth-latency ("" = default, kDefaultEthLatency).  Needs `clock`.
	std::string ethLatency;
	// The emulated clock the latency is measured on: now() is monotonic in
	// units of 1/unitsPerSecond s.  Unset (tests, tools): no latency is added.
	std::function<uint64_t()> clock;
	uint64_t clockUnitsPerSecond = 0;
	// Headless or scripted run.  Such runs must be repeatable, and slirp runs
	// on the host clock and real sockets, so with no --eth given an unattended
	// run gets `none` (cable out) instead of slirp.  Ask for a live link with
	// an explicit --eth=slirp.  Interactive runs default to slirp.
	bool unattended = false;
};

// Minimum delay on received frames when --eth-latency is not given.
inline constexpr const char *kDefaultEthLatency = "10ms";

// `type` is "daynaport[,multiframe]"; "" and "none" give nullptr.  Unknown
// types warn on stderr and give nullptr; unknown options warn and are ignored.
// daynaport options: multiframe = pack several frames per READ (default: one,
// as BlueSCSI does; unverified against the real driver).
std::unique_ptr<scsi::ScsiTarget> CreateScsiTarget(const std::string &type, int id,
													const ScsiTargetOptions &opt);
