// Integration helper for ctest "eth_hub_ethhub": three EthHub backends on one
// real ethhub (HOST:PORT on the command line) check unicast and broadcast
// delivery in both directions.  Exit 0 = all good.  Driven by eth_hub_it.py.
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "devices/eth_hub.h"
#include "devices/eth_hub_auth.h"

namespace
{
using Mac = std::array<uint8_t, 6>;
constexpr Mac kBcast = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

std::vector<uint8_t> frame(const Mac &dst, const Mac &src, uint8_t tag)
{
	std::vector<uint8_t> f(60, tag);
	std::copy(dst.begin(), dst.end(), f.begin());
	std::copy(src.begin(), src.end(), f.begin() + 6);
	f[12] = 0x88;
	f[13] = 0xB5; // local experimental ethertype
	return f;
}

struct Node
{
	const char *name;
	Mac mac;
	std::unique_ptr<EthHub> hub;
};

int failures = 0;
void check(bool ok, const char *what)
{
	printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) failures++;
}

// Poll every node for `ms`; collect the frames each one receives.
std::vector<std::vector<std::vector<uint8_t>>> pump(std::vector<Node> &nodes, int ms)
{
	std::vector<std::vector<std::vector<uint8_t>>> got(nodes.size());
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < end)
	{
		for (size_t i = 0; i < nodes.size(); i++)
		{
			nodes[i].hub->poll();
			while (const size_t n = nodes[i].hub->peekSize())
			{
				std::vector<uint8_t> f(n);
				nodes[i].hub->receive(f);
				got[i].push_back(std::move(f));
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return got;
}
} // namespace

int main(int argc, char **argv)
{
	if (argc != 2 && argc != 3)
	{
		fprintf(stderr, "usage: eth-hub-it HOST:PORT [KEYFILE]\n");
		return 2;
	}
	EthHub::Options opt;
	if (argc == 3)
	{
		std::string kerr;
		if (!loadHubKey(argv[2], opt.key, kerr))
		{
			fprintf(stderr, "%s\n", kerr.c_str());
			return 2;
		}
	}
	std::vector<Node> n;
	const Mac macs[3] = {{0x02, 0, 0, 0, 0, 0x0A}, {0x02, 0, 0, 0, 0, 0x0B}, {0x02, 0, 0, 0, 0, 0x0C}};
	const char *names[3] = {"A", "B", "C"};
	for (int i = 0; i < 3; i++)
	{
		std::string err;
		auto h = EthHub::create(argv[1], err, opt);
		if (!h)
		{
			fprintf(stderr, "%s\n", err.c_str());
			return 2;
		}
		n.push_back({names[i], macs[i], std::move(h)});
	}
	pump(n, 200); // let the hellos arrive

	// 1. B broadcasts: A and C get it, B (the sender's port) does not.
	{
		const auto f = frame(kBcast, n[1].mac, 1);
		check(n[1].hub->send(f), "B sends a broadcast");
		auto g = pump(n, 400);
		check(g[0].size() == 1 && g[0][0] == f, "broadcast from B reaches A, byte for byte");
		check(g[2].size() == 1 && g[2][0] == f, "broadcast from B reaches C");
		check(g[1].empty(), "broadcast is not echoed back to B");
	}
	// 2. A unicast to B (the hub learned B's MAC from step 1): only B gets it.
	{
		const auto f = frame(n[1].mac, n[0].mac, 2);
		check(n[0].hub->send(f), "A sends a unicast to B");
		auto g = pump(n, 400);
		check(g[1].size() == 1 && g[1][0] == f, "unicast A->B reaches B");
		check(g[0].empty() && g[2].empty(), "unicast A->B reaches nobody else");
	}
	// 3. B unicast to A (A's MAC was learned from step 2).
	{
		const auto f = frame(n[0].mac, n[1].mac, 3);
		check(n[1].hub->send(f), "B sends a unicast to A");
		auto g = pump(n, 400);
		check(g[0].size() == 1 && g[0][0] == f, "unicast B->A reaches A");
		check(g[1].empty() && g[2].empty(), "unicast B->A reaches nobody else");
	}
	// 4. A broadcasts: B and C get it.
	{
		const auto f = frame(kBcast, n[0].mac, 4);
		check(n[0].hub->send(f), "A sends a broadcast");
		auto g = pump(n, 400);
		check(g[1].size() == 1 && g[1][0] == f && g[2].size() == 1 && g[2][0] == f, "broadcast from A reaches B and C");
		check(g[0].empty(), "broadcast is not echoed back to A");
	}
	// 5. unknown unicast floods to the others.
	{
		const Mac nobody = {0x02, 0, 0, 0, 0, 0x77};
		const auto f = frame(nobody, n[2].mac, 5);
		n[2].hub->send(f);
		auto g = pump(n, 400);
		check(g[0].size() == 1 && g[1].size() == 1 && g[2].empty(), "unknown unicast from C floods to A and B");
	}
	printf("%s\n", failures ? "FAILED" : "ALL OK");
	return failures ? 1 : 0;
}
