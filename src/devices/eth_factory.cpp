/*
	eth_factory.cpp — see eth_factory.h.
*/

#include "devices/eth_factory.h"

#include <cstdio>
#include <functional>
#include <map>

#include "devices/eth_null.h"
#include "devices/eth_spec.h"
#if HAVE_SLIRP
#include "devices/eth_slirp.h"
#endif
#if HAVE_ETH_HUB
#include "devices/eth_hub.h"
#endif

namespace
{
using Creator = std::function<std::unique_ptr<EthernetBackend>(const EthSpec &, const std::vector<HostFwdSpec> &)>;

#if HAVE_SLIRP
std::unique_ptr<EthernetBackend> makeSlirp(const EthSpec &spec, const std::vector<HostFwdSpec> &extra)
{
	auto e = std::make_unique<EthSlirp>();
	auto fwd = [&](const HostFwdSpec &f) {
		if (!e->addHostFwd(f.udp, f.hostIp.c_str(), f.hostPort, f.guestIp.c_str(), f.guestPort))
			fprintf(stderr, "[ETH] host forward %s:%s:%u -> %s:%u failed\n", f.udp ? "udp" : "tcp",
					f.hostIp.empty() ? "127.0.0.1" : f.hostIp.c_str(), f.hostPort,
					f.guestIp.empty() ? "guest" : f.guestIp.c_str(), f.guestPort);
	};
	for (const auto &f : spec.hostfwds) fwd(f);
	for (const auto &f : extra) fwd(f);
	return e;
}
#endif

#if HAVE_ETH_HUB
std::unique_ptr<EthernetBackend> makeHub(const EthSpec &spec, const std::vector<HostFwdSpec> &extra)
{
	if (!extra.empty() || !spec.hostfwds.empty())
		fprintf(stderr, "[ETH] hub: host forwards do not apply to a hub; ignored\n");
	std::string err;
	auto h = EthHub::create(spec.arg, err);
	if (!h)
	{
		fprintf(stderr, "[ETH] %s; using no network\n", err.c_str());
		return std::make_unique<EthNull>();
	}
	return h;
}
#endif

const std::map<std::string, Creator> &registry()
{
	static const std::map<std::string, Creator> r = {
		{"none", [](const EthSpec &, const std::vector<HostFwdSpec> &) -> std::unique_ptr<EthernetBackend> {
			 return std::make_unique<EthNull>();
		 }},
#if HAVE_SLIRP
		{"slirp", makeSlirp},
#endif
#if HAVE_ETH_HUB
		{"hub", makeHub},
#endif
	};
	return r;
}
} // namespace

std::unique_ptr<EthernetBackend> CreateEthBackend(const std::string &spec,
												   const std::vector<std::string> &redirs)
{
	std::string s = spec;
#if HAVE_SLIRP
	if (s.empty()) s = "slirp";
#else
	if (s.empty()) s = "none";
#endif
	EthSpec es;
	std::string err;
	if (!parseEthSpec(s, es, err))
	{
		fprintf(stderr, "[ETH] %s; using no network\n", err.c_str());
		return std::make_unique<EthNull>();
	}
	auto it = registry().find(es.kind);
	if (it == registry().end())
	{
		fprintf(stderr, "[ETH] unknown or unavailable --eth kind '%s'; using no network\n", es.kind.c_str());
		return std::make_unique<EthNull>();
	}
	std::vector<HostFwdSpec> extra;
	for (const auto &r : redirs)
	{
		HostFwdSpec f;
		if (parseNetRedir(r, f)) extra.push_back(f);
		else fprintf(stderr, "[ETH] bad --net-redir spec: %s\n", r.c_str());
	}
	fprintf(stderr, "[ETH] backend: %s\n", es.kind.c_str());
	return it->second(es, extra);
}
