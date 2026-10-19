/*
	scsi_factory.cpp — see scsi_factory.h.
*/

#include "devices/scsi_factory.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "devices/eth_factory.h"
#include "devices/eth_spec.h"
#include "devices/scsi_daynaport.h"

std::unique_ptr<scsi::ScsiTarget> CreateScsiTarget(const std::string &typeSpec, int id,
													const ScsiTargetOptions &opt)
{
	// "type[,option]..."
	std::vector<std::string> parts;
	for (size_t i = 0; i <= typeSpec.size();)
	{
		const size_t j = std::min(typeSpec.find(',', i), typeSpec.size());
		parts.push_back(typeSpec.substr(i, j - i));
		i = j + 1;
	}
	const std::string &type = parts[0];
	if (type.empty() || type == "none") return nullptr;
	if (type == "daynaport")
	{
		DaynaPortTarget::Options topt;
		for (size_t i = 1; i < parts.size(); i++)
		{
			if (parts[i] == "multiframe") topt.multiFrame = true;
			else fprintf(stderr, "[SCSI] ID %d: unknown daynaport option '%s' (known: multiframe)\n", id, parts[i].c_str());
		}
		DaynaPortTarget::Mac mac = DaynaPortTarget::kDefaultMac;
		if (!opt.ethMac.empty() && !parseMac(opt.ethMac, mac))
		{
			fprintf(stderr, "[SCSI] bad --eth-mac '%s' (need a unicast xx:xx:xx:xx:xx:xx); using default\n",
					opt.ethMac.c_str());
			mac = DaynaPortTarget::kDefaultMac;
		}
		std::string ethSpec = opt.eth;
		if (ethSpec.empty() && opt.unattended)
		{
			ethSpec = "none";
			fprintf(stderr, "[SCSI] ID %d: headless/script run without --eth: link down (--eth=slirp for a live link)\n", id);
		}
		auto eth = CreateEthBackend(ethSpec, opt.redirs);
		fprintf(stderr, "[SCSI] ID %d: DaynaPORT SCSI/Link, MAC %02x:%02x:%02x:%02x:%02x:%02x, backend %s\n", id,
				mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], eth->name());
		return std::make_unique<DaynaPortTarget>(std::move(eth), mac, topt);
	}
	fprintf(stderr, "[SCSI] ID %d: unknown target type '%s' (known: daynaport, none)\n", id, type.c_str());
	return nullptr;
}
