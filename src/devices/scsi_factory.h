/*
	scsi_factory — create SCSI targets from --scsiN=TYPE strings.
*/
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "devices/scsi_target.h"

struct ScsiTargetOptions
{
	std::string eth;				  // --eth spec ("" = default)
	std::vector<std::string> redirs;  // --net-redir specs
	std::string ethMac;				  // --eth-mac ("" = default MAC)
};

// `type` is "daynaport"; "" and "none" give nullptr.  Unknown types warn on
// stderr and give nullptr.
std::unique_ptr<scsi::ScsiTarget> CreateScsiTarget(const std::string &type, int id,
													const ScsiTargetOptions &opt);
