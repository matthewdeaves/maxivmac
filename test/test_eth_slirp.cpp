// EthSlirp against libslirp itself.  Only offline, loopback-free exchanges:
// the guest talks to slirp's own gateway (ARP, ICMP echo), nothing leaves
// the machine.  Compiled only when libslirp is available.
#include <doctest/doctest.h>

#if HAVE_SLIRP

#include <vector>

#include "devices/eth_slirp.h"

namespace
{
using Bytes = std::vector<uint8_t>;
const uint8_t kGuestMac[6] = {0x02, 0x6D, 0x78, 0x76, 0x6D, 0x01};

uint16_t checksum(const uint8_t *p, size_t n)
{
	uint32_t s = 0;
	for (size_t i = 0; i < n; i += 2) s += (p[i] << 8) | (i + 1 < n ? p[i + 1] : 0);
	while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
	return uint16_t(~s);
}

Bytes arpRequest(const uint8_t targetIp[4])
{
	Bytes f(42, 0);
	for (int i = 0; i < 6; i++) f[i] = 0xFF;
	for (int i = 0; i < 6; i++) f[6 + i] = kGuestMac[i];
	f[12] = 0x08;
	f[13] = 0x06;
	const uint8_t hdr[] = {0, 1, 8, 0, 6, 4, 0, 1};
	for (int i = 0; i < 8; i++) f[14 + i] = hdr[i];
	for (int i = 0; i < 6; i++) f[22 + i] = kGuestMac[i];
	const uint8_t ip[4] = {10, 0, 2, 15};
	for (int i = 0; i < 4; i++) f[28 + i] = ip[i], f[38 + i] = targetIp[i];
	return f;
}

Bytes pingGateway(const uint8_t gwMac[6])
{
	Bytes f(14 + 28, 0);
	for (int i = 0; i < 6; i++) f[i] = gwMac[i], f[6 + i] = kGuestMac[i];
	f[12] = 0x08;
	const uint8_t ip[] = {0x45, 0, 0, 28, 0, 1, 0, 0, 64, 1, 0, 0, 10, 0, 2, 15, 10, 0, 2, 2};
	for (int i = 0; i < 20; i++) f[14 + i] = ip[i];
	const uint16_t c = checksum(&f[14], 20);
	f[24] = uint8_t(c >> 8);
	f[25] = uint8_t(c);
	const uint8_t icmp[] = {8, 0, 0, 0, 0, 1, 0, 1};
	for (int i = 0; i < 8; i++) f[34 + i] = icmp[i];
	const uint16_t cc = checksum(&f[34], 8);
	f[36] = uint8_t(cc >> 8);
	f[37] = uint8_t(cc);
	return f;
}

// Poll until a frame with this EtherType arrives (or give up).
bool waitFrame(EthSlirp &e, uint16_t type, Bytes &out)
{
	for (int i = 0; i < 300; i++)
	{
		e.poll();
		while (e.peekSize())
		{
			Bytes f(e.peekSize());
			e.receive(f);
			if (f.size() >= 14 && ((f[12] << 8) | f[13]) == type)
			{
				out = f;
				return true;
			}
		}
	}
	return false;
}
} // namespace

TEST_CASE("eth slirp: link is up and bad frame sizes are refused")
{
	EthSlirp e;
	CHECK(e.linkUp());
	CHECK(std::string(e.name()) == "slirp");
	CHECK_FALSE(e.send(Bytes(13, 0)));
	CHECK_FALSE(e.send(Bytes(1515, 0)));
	CHECK(e.peekSize() == 0);
}

TEST_CASE("eth slirp: ARP for the gateway is answered with slirp's MAC")
{
	EthSlirp e;
	const uint8_t gw[4] = {10, 0, 2, 2};
	REQUIRE(e.send(arpRequest(gw)));
	Bytes r;
	REQUIRE(waitFrame(e, 0x0806, r));
	CHECK(r[21] == 2); // ARP reply
	const auto gwMac = SlirpStack::Config::macFor(0x0A000202);
	CHECK(Bytes(r.begin() + 22, r.begin() + 28) == Bytes(gwMac.begin(), gwMac.end()));
	CHECK(Bytes(r.begin() + 28, r.begin() + 32) == Bytes(gw, gw + 4));
}

TEST_CASE("eth slirp: ICMP echo to the gateway gets a reply")
{
	EthSlirp e;
	const auto gwMac = SlirpStack::Config::macFor(0x0A000202);
	// slirp learns the guest's MAC from the guest's own ARP request.
	const uint8_t gw[4] = {10, 0, 2, 2};
	REQUIRE(e.send(arpRequest(gw)));
	Bytes r;
	REQUIRE(waitFrame(e, 0x0806, r));
	REQUIRE(e.send(pingGateway(gwMac.data())));
	REQUIRE(waitFrame(e, 0x0800, r));
	CHECK(r[14 + 9] == 1);	// ICMP
	CHECK(r[34] == 0);		// echo reply
	CHECK(Bytes(r.begin() + 26, r.begin() + 30) == Bytes({10, 0, 2, 2}));
}

TEST_CASE("eth slirp: custom addressing comes from one Config")
{
	SlirpStack::Config c;
	c.network = 0xC0A80000; // 192.168.0.0/24
	c.netmask = 0xFFFFFF00;
	c.gateway = 0xC0A80001;
	c.dns = 0xC0A80003;
	c.guest = 0xC0A8000F;
	EthSlirp e(c);
	const uint8_t gw[4] = {192, 168, 0, 1};
	Bytes req = arpRequest(gw);
	for (int i = 0; i < 4; i++) req[28 + i] = uint8_t((c.guest >> (24 - 8 * i)) & 0xFF);
	REQUIRE(e.send(req));
	Bytes r;
	REQUIRE(waitFrame(e, 0x0806, r));
	CHECK(r[22] == 0x52);
	CHECK(r[27] == 0x01); // 52:55:c0:a8:00:01
}

TEST_CASE("eth slirp: host forwards reject a bad guest address")
{
	EthSlirp e;
	CHECK_FALSE(e.addHostFwd(false, 0, "not-an-ip", 80));
}

#endif // HAVE_SLIRP
