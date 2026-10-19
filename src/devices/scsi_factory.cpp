/*
	scsi_factory.cpp — see scsi_factory.h.
*/

#include "devices/scsi_factory.h"

#include <cstdio>

#include "devices/eth_factory.h"
#include "devices/eth_spec.h"
#include "devices/scsi_daynaport.h"

std::unique_ptr<scsi::ScsiTarget> CreateScsiTarget(const std::string &type, int id,
													const ScsiTargetOptions &opt)
{
	if (type.empty() || type == "none") return nullptr;
	if (type == "daynaport")
	{
		DaynaPortTarget::Mac mac = DaynaPortTarget::kDefaultMac;
		if (!opt.ethMac.empty() && !parseMac(opt.ethMac, mac))
		{
			fprintf(stderr, "[SCSI] bad --eth-mac '%s' (need a unicast xx:xx:xx:xx:xx:xx); using default\n",
					opt.ethMac.c_str());
			mac = DaynaPortTarget::kDefaultMac;
		}
		auto eth = CreateEthBackend(opt.eth, opt.redirs);
		fprintf(stderr, "[SCSI] ID %d: DaynaPORT SCSI/Link, MAC %02x:%02x:%02x:%02x:%02x:%02x, backend %s\n", id,
				mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], eth->name());
		return std::make_unique<DaynaPortTarget>(std::move(eth), mac);
	}
	fprintf(stderr, "[SCSI] ID %d: unknown target type '%s' (known: daynaport, none)\n", id, type.c_str());
	return nullptr;
}
