// Tests for --eth / --eth-mac / --net-redir parsing and the backend factory.
#include <doctest/doctest.h>

#include "devices/eth_factory.h"
#include "devices/eth_spec.h"

TEST_CASE("eth spec: hostfwd tcp and udp with and without a guest address")
{
	HostFwdSpec f;
	REQUIRE(parseHostFwd("tcp::8080-10.0.2.15:80", f));
	CHECK_FALSE(f.udp);
	CHECK(f.hostPort == 8080);
	CHECK(f.guestIp == "10.0.2.15");
	CHECK(f.guestPort == 80);
	REQUIRE(parseHostFwd("udp::5000-:5001", f));
	CHECK(f.udp);
	CHECK(f.guestIp.empty());
	CHECK(f.guestPort == 5001);
}

TEST_CASE("eth spec: hostfwd carries an optional host address, default loopback")
{
	HostFwdSpec f;
	REQUIRE(parseHostFwd("tcp::8080-:80", f));
	CHECK(f.hostIp.empty()); // empty means 127.0.0.1 when applied
	REQUIRE(parseHostFwd("tcp:127.0.0.1:8080-10.0.2.15:80", f));
	CHECK(f.hostIp == "127.0.0.1");
	REQUIRE(parseHostFwd("udp:0.0.0.0:5000-:5001", f));
	CHECK(f.hostIp == "0.0.0.0");
	CHECK(f.hostPort == 5000);
	CHECK_FALSE(parseHostFwd("tcp:localhost:8080-:80", f)); // dotted quad only
	CHECK_FALSE(parseHostFwd("tcp:1.2.3:8080-:80", f));
	REQUIRE(parseNetRedir("tcp:8080:10.0.2.15:80", f));
	CHECK(f.hostIp.empty()); // the legacy form is loopback only
}

TEST_CASE("eth spec: malformed hostfwd is rejected")
{
	HostFwdSpec f;
	for (const char *bad : {"", "tcp", "tcp:8080-10.0.2.15:80", "tcp::8080:10.0.2.15:80", "tcp::x-10.0.2.15:80",
							"tcp::70000-10.0.2.15:80", "tcp::8080-10.0.2:80", "tcp::8080-10.0.2.15:", "sctp::1-:2",
							"tcp::8080-10.0.2.256:80"})
		CHECK_MESSAGE(!parseHostFwd(bad, f), bad);
}

TEST_CASE("eth spec: the legacy --slip-redir form still parses")
{
	HostFwdSpec f;
	REQUIRE(parseNetRedir("tcp:8080:10.0.2.15:80", f));
	CHECK(f.hostPort == 8080);
	CHECK(f.guestIp == "10.0.2.15");
	CHECK(f.guestPort == 80);
	REQUIRE(parseNetRedir("udp:53:10.0.2.15:53", f));
	CHECK(f.udp);
	CHECK_FALSE(parseNetRedir("tcp:8080:10.0.2.15", f));
	CHECK_FALSE(parseNetRedir("8080:10.0.2.15:80", f));
}

TEST_CASE("eth spec: MAC parsing")
{
	std::array<uint8_t, 6> m{};
	REQUIRE(parseMac("02:00:00:aa:BB:cc", m));
	CHECK(m == std::array<uint8_t, 6>{2, 0, 0, 0xAA, 0xBB, 0xCC});
	CHECK(parseMac("02-00-00-aa-bb-cc", m));
	CHECK_FALSE(parseMac("01:00:5e:00:00:01", m)); // multicast
	CHECK_FALSE(parseMac("02:00:00:aa:bb", m));
	CHECK_FALSE(parseMac("02:00:00:aa:bb:gg", m));
	CHECK_FALSE(parseMac("02:00:00:aa:bb:cc:dd", m));
	CHECK_FALSE(parseMac("", m));
}

TEST_CASE("eth spec: kind, argument and options")
{
	EthSpec s;
	std::string err;
	REQUIRE(parseEthSpec("slirp", s, err));
	CHECK(s.kind == "slirp");
	CHECK(s.arg.empty());
	REQUIRE(parseEthSpec("slirp,hostfwd=tcp::8080-:80,hostfwd=udp::53-10.0.2.15:53", s, err));
	CHECK(s.hostfwds.size() == 2);
	CHECK(s.options.size() == 2);
	REQUIRE(parseEthSpec("hub:example.org:9000,foo=bar", s, err)); // a future kind parses already
	CHECK(s.kind == "hub");
	CHECK(s.arg == "example.org:9000");
	CHECK(s.options.size() == 1);
}

TEST_CASE("eth spec: bad specs report an error")
{
	EthSpec s;
	std::string err;
	CHECK_FALSE(parseEthSpec("", s, err));
	CHECK_FALSE(parseEthSpec(",hostfwd=x", s, err));
	CHECK_FALSE(parseEthSpec("slirp,hostfwd", s, err));
	CHECK_FALSE(parseEthSpec("slirp,hostfwd=tcp::bad", s, err));
	CHECK(err.find("hostfwd") != std::string::npos);
}

TEST_CASE("eth factory: none gives a link-down backend; unknown kinds fall back to none")
{
	auto n = CreateEthBackend("none", {});
	REQUIRE(n);
	CHECK_FALSE(n->linkUp());
	CHECK_FALSE(n->send(std::vector<uint8_t>(60, 0)));
	CHECK(n->peekSize() == 0);
	auto u = CreateEthBackend("carrier-pigeon", {});
	REQUIRE(u);
	CHECK_FALSE(u->linkUp());
	auto bad = CreateEthBackend("slirp,hostfwd=nonsense", {});
	REQUIRE(bad);
	CHECK_FALSE(bad->linkUp());
}
