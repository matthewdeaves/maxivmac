/*
	scsi_factory.cpp — see scsi_factory.h.
*/

#include "devices/scsi_factory.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

#include "devices/eth_factory.h"
#include "devices/eth_latency.h"
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
		if (eth->sharesLan() && opt.ethMac.empty())
		{
			// Every emulator on a shared hub would otherwise be 02:6d:78:76:6d:01
			// and the hub would silently swap their traffic.  Pick the last three
			// octets at random (24 bits: collisions among a handful of emulators
			// are negligible) and say so; --eth-mac makes it repeatable.
			std::random_device rd;
			const uint32_t r = rd();
			mac = {0x02, 0x6D, 0x78, uint8_t(r >> 16), uint8_t(r >> 8), uint8_t(r)};
			fprintf(stderr, "[SCSI] ID %d: shared LAN and no --eth-mac: random MAC %02x:%02x:%02x:%02x:%02x:%02x\n", id,
					mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		}
		if (opt.clock && opt.clockUnitsPerSecond)
		{
			uint64_t lat = 0;
			std::string err;
			if (!parseEthLatency(opt.ethLatency.empty() ? kDefaultEthLatency : opt.ethLatency,
								 opt.clockUnitsPerSecond, lat, err))
			{
				fprintf(stderr, "[SCSI] ID %d: %s; using %s\n", id, err.c_str(), kDefaultEthLatency);
				parseEthLatency(kDefaultEthLatency, opt.clockUnitsPerSecond, lat, err);
			}
			if (lat)
			{
				fprintf(stderr, "[SCSI] ID %d: received frames held for %s of emulated time (--eth-latency)\n", id,
						opt.ethLatency.empty() ? kDefaultEthLatency : opt.ethLatency.c_str());
				eth = std::make_unique<EthLatency>(std::move(eth), opt.clock, lat);
			}
		}
		fprintf(stderr, "[SCSI] ID %d: DaynaPORT SCSI/Link, MAC %02x:%02x:%02x:%02x:%02x:%02x, backend %s\n", id,
				mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], eth->name());
		return std::make_unique<DaynaPortTarget>(std::move(eth), mac, topt);
	}
	fprintf(stderr, "[SCSI] ID %d: unknown target type '%s' (known: daynaport, none)\n", id, type.c_str());
	return nullptr;
}
