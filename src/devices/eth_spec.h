/*
	eth_spec.h — parsing of --eth / --eth-mac / --net-redir strings.

	Pure string handling, no libslirp: unit-testable everywhere.

	  --eth=KIND[:ARG][,key=value]...      e.g.  slirp,hostfwd=tcp::8080-10.0.2.15:80
	                                             hub:127.0.0.1:5555[,key=FILE]   (FILE: ethhub shared secret, mode 0600)
	  hostfwd=tcp:[HOSTIP]:HOSTPORT-[GUESTIP]:GUESTPORT   (also udp)
	      HOSTIP  host address to listen on; empty = 127.0.0.1 (this machine
	              only).  0.0.0.0 exposes the forward on every interface.
	      GUESTIP empty = the guest address
	  --net-redir=tcp:HOSTPORT:GUESTIP:GUESTPORT   (legacy form, also udp)
	  --eth-mac=xx:xx:xx:xx:xx:xx
	  --eth-latency=2ms | 500us | 0 | off
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
	std::string hostIp; // empty = loopback (127.0.0.1)
	uint16_t hostPort = 0;
	std::string guestIp; // empty = the stack's guest address
	uint16_t guestPort = 0;
};

// "tcp::8080-10.0.2.15:80".  Returns false on a malformed spec.
bool parseHostFwd(std::string_view s, HostFwdSpec &out);
// "tcp:[HOSTIP:]8080:10.0.2.15:80" (--net-redir / --slip-redir).  Without a
// HOSTIP, hostIp stays empty, which the stack treats as loopback only.
bool parseNetRedir(std::string_view s, HostFwdSpec &out);
// --slip-redir kept upstream's behaviour of listening on every interface.  This
// rewrites a spec without a HOSTIP to an explicit 0.0.0.0 one (compat); a spec
// that already names a HOSTIP, or does not parse, is returned unchanged.
std::string legacyRedirAllInterfaces(const std::string &spec);
// "02:00:00:aa:bb:cc" (':' or '-'); rejects multicast (bit 0 of the first octet).
bool parseMac(std::string_view s, std::array<uint8_t, 6> &out);

// --eth-latency=DURATION: minimum delay before a received frame is visible to
// the guest, on the emulated clock.  "2ms", "500us", "1s", or "0"/"off"/"none"
// for no delay.  A bare non-zero number is refused (no unit to read it in), as
// are the raw-cycle suffixes of script durations.  `unitsPerSecond` is the
// emulated clock's rate; `out` is in those units.  At most 1 s.
bool parseEthLatency(std::string_view s, uint64_t unitsPerSecond, uint64_t &out, std::string &err);

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
