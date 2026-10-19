/*
	eth_spec.cpp — see eth_spec.h.
*/

#include "devices/eth_spec.h"

#include <cctype>
#include <charconv>

namespace
{
bool parsePort(std::string_view s, uint16_t &out)
{
	unsigned v = 0;
	if (s.empty()) return false;
	auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
	if (ec != std::errc() || p != s.data() + s.size() || v > 65535) return false;
	out = uint16_t(v);
	return true;
}

bool parseProto(std::string_view &s, bool &udp)
{
	if (s.substr(0, 4) == "tcp:") udp = false;
	else if (s.substr(0, 4) == "udp:") udp = true;
	else return false;
	s.remove_prefix(4);
	return true;
}

bool validIpv4(std::string_view s)
{
	int parts = 0;
	size_t i = 0;
	while (i <= s.size())
	{
		size_t j = s.find('.', i);
		if (j == std::string_view::npos) j = s.size();
		unsigned v = 0;
		std::string_view p = s.substr(i, j - i);
		if (p.empty() || p.size() > 3) return false;
		auto [e, ec] = std::from_chars(p.data(), p.data() + p.size(), v);
		if (ec != std::errc() || e != p.data() + p.size() || v > 255) return false;
		parts++;
		i = j + 1;
	}
	return parts == 4;
}
} // namespace

bool parseHostFwd(std::string_view s, HostFwdSpec &out)
{
	HostFwdSpec r;
	if (!parseProto(s, r.udp)) return false;
	// Now "[hostaddr]:hostport-[guestaddr]:guestport".
	const size_t hc = s.find(':');
	if (hc == std::string_view::npos) return false;
	const std::string_view host = s.substr(0, hc);
	if (!host.empty() && !validIpv4(host)) return false;
	r.hostIp = std::string(host);
	s.remove_prefix(hc + 1);
	const size_t dash = s.find('-');
	if (dash == std::string_view::npos) return false;
	if (!parsePort(s.substr(0, dash), r.hostPort)) return false;
	s.remove_prefix(dash + 1);
	const size_t colon = s.rfind(':');
	if (colon == std::string_view::npos) return false;
	const std::string_view ip = s.substr(0, colon);
	if (!ip.empty() && !validIpv4(ip)) return false;
	r.guestIp = std::string(ip);
	if (!parsePort(s.substr(colon + 1), r.guestPort)) return false;
	out = r;
	return true;
}

bool parseNetRedir(std::string_view s, HostFwdSpec &out)
{
	HostFwdSpec r;
	if (!parseProto(s, r.udp)) return false;
	const size_t c1 = s.find(':');
	const size_t c2 = s.rfind(':');
	if (c1 == std::string_view::npos || c2 == c1) return false;
	if (!parsePort(s.substr(0, c1), r.hostPort)) return false;
	const std::string_view ip = s.substr(c1 + 1, c2 - c1 - 1);
	if (!validIpv4(ip)) return false;
	r.guestIp = std::string(ip);
	if (!parsePort(s.substr(c2 + 1), r.guestPort)) return false;
	out = r;
	return true;
}

bool parseMac(std::string_view s, std::array<uint8_t, 6> &out)
{
	if (s.size() != 17) return false;
	std::array<uint8_t, 6> m{};
	for (int i = 0; i < 6; i++)
	{
		const std::string_view p = s.substr(size_t(i) * 3, 2);
		if (i < 5 && s[size_t(i) * 3 + 2] != ':' && s[size_t(i) * 3 + 2] != '-') return false;
		unsigned v = 0;
		auto [e, ec] = std::from_chars(p.data(), p.data() + 2, v, 16);
		if (ec != std::errc() || e != p.data() + 2) return false;
		m[size_t(i)] = uint8_t(v);
	}
	if (m[0] & 1) return false; // group address cannot be a station address
	out = m;
	return true;
}

bool parseEthSpec(std::string_view s, EthSpec &out, std::string &err)
{
	EthSpec r;
	if (s.empty())
	{
		err = "empty --eth spec";
		return false;
	}
	size_t comma = s.find(',');
	std::string_view head = s.substr(0, comma);
	const size_t colon = head.find(':');
	r.kind = std::string(head.substr(0, colon));
	if (colon != std::string_view::npos) r.arg = std::string(head.substr(colon + 1));
	if (r.kind.empty())
	{
		err = "--eth needs a kind (slirp, none)";
		return false;
	}
	while (comma != std::string_view::npos)
	{
		s.remove_prefix(comma + 1);
		comma = s.find(',');
		const std::string_view opt = s.substr(0, comma);
		const size_t eq = opt.find('=');
		if (opt.empty() || eq == std::string_view::npos || eq == 0)
		{
			err = "bad --eth option '" + std::string(opt) + "' (expected key=value)";
			return false;
		}
		const std::string key(opt.substr(0, eq)), val(opt.substr(eq + 1));
		if (key == "hostfwd")
		{
			HostFwdSpec f;
			if (!parseHostFwd(val, f))
			{
				err = "bad hostfwd '" + val + "' (expected tcp::HOSTPORT-GUESTIP:GUESTPORT)";
				return false;
			}
			r.hostfwds.push_back(f);
		}
		r.options.emplace_back(key, val);
	}
	out = std::move(r);
	return true;
}
