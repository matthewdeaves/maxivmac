/*
	eth_spec.h — parsing of --eth / --eth-mac / --net-redir strings.

	Pure string handling, no libslirp: unit-testable everywhere.

	  --eth=KIND[:ARG][,key=value]...      e.g.  slirp,hostfwd=tcp::8080-10.0.2.15:80
	  hostfwd=tcp::HOSTPORT-GUESTIP:GUESTPORT   (also udp; GUESTIP may be empty
	                                             = the guest address)
	  --net-redir=tcp:HOSTPORT:GUESTIP:GUESTPORT   (legacy form, also udp)
	  --eth-mac=xx:xx:xx:xx:xx:xx
*/
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct HostFwdSpec
{
	bool udp = false;
	uint16_t hostPort = 0;
	std::string guestIp; // empty = the stack's guest address
	uint16_t guestPort = 0;
};

// "tcp::8080-10.0.2.15:80".  Returns false on a malformed spec.
bool parseHostFwd(std::string_view s, HostFwdSpec &out);
// "tcp:8080:10.0.2.15:80" (the former --slip-redir form).
bool parseNetRedir(std::string_view s, HostFwdSpec &out);
// "02:00:00:aa:bb:cc" (':' or '-'); rejects multicast (bit 0 of the first octet).
bool parseMac(std::string_view s, std::array<uint8_t, 6> &out);

struct EthSpec
{
	std::string kind; // "slirp", "none", ...
	std::string arg;  // text after "kind:" up to the first ','; may be empty
	std::vector<std::pair<std::string, std::string>> options; // key=value, in order
	std::vector<HostFwdSpec> hostfwds;						   // parsed from hostfwd= options
};

// Returns false and sets `err` on a malformed spec.  An empty string is an
// error; callers decide what "no --eth" means.
bool parseEthSpec(std::string_view s, EthSpec &out, std::string &err);
