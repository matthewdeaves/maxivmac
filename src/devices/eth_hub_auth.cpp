/*
	eth_hub_auth.cpp — see eth_hub_auth.h.  SHA-256 per FIPS 180-4, HMAC per RFC 2104.
*/

#include "devices/eth_hub_auth.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256
{
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	uint8_t buf[64] = {};
	uint64_t len = 0;
	size_t fill = 0;

	void block(const uint8_t *p)
	{
		uint32_t w[64];
		for (size_t i = 0; i < 16; i++)
		{
			const uint8_t *q = p + 4 * i;
			w[i] = uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3];
		}
		for (int i = 16; i < 64; i++)
		{
			const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++)
		{
			const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
			const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
	}

	void update(const uint8_t *p, size_t n)
	{
		len += n;
		while (n > 0)
		{
			const size_t k = std::min(64 - fill, n);
			std::memcpy(buf + fill, p, k);
			fill += k; p += k; n -= k;
			if (fill == 64) { block(buf); fill = 0; }
		}
	}

	std::array<uint8_t, 32> finish()
	{
		const uint64_t bits = len * 8;
		const uint8_t one = 0x80, zero = 0;
		update(&one, 1);
		while (fill != 56) update(&zero, 1);
		uint8_t lb[8];
		for (int i = 0; i < 8; i++) lb[i] = uint8_t(bits >> (56 - 8 * i));
		update(lb, 8);
		std::array<uint8_t, 32> out{};
		for (size_t i = 0; i < 8; i++)
			for (size_t j = 0; j < 4; j++) out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
		return out;
	}
};
} // namespace

std::array<uint8_t, 32> hmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> msg)
{
	uint8_t k[64] = {};
	if (key.size() > 64)
	{
		Sha256 s;
		s.update(key.data(), key.size());
		const auto d = s.finish();
		std::memcpy(k, d.data(), d.size());
	}
	else if (!key.empty())
	{
		std::memcpy(k, key.data(), key.size());
	}
	uint8_t ipad[64], opad[64];
	for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
	Sha256 in;
	in.update(ipad, 64);
	in.update(msg.data(), msg.size());
	const auto inner = in.finish();
	Sha256 out;
	out.update(opad, 64);
	out.update(inner.data(), inner.size());
	return out.finish();
}

std::array<uint8_t, kHubHelloLen> makeHubHello(std::span<const uint8_t> key, uint64_t unixSeconds)
{
	static constexpr char kCtx[] = "ethhub-hello-v1";
	constexpr size_t kCtxLen = sizeof kCtx - 1;
	std::array<uint8_t, kHubHelloLen> h{};
	const uint32_t t = uint32_t(unixSeconds);
	h[0] = 0xE7;
	h[1] = uint8_t(t >> 24); h[2] = uint8_t(t >> 16); h[3] = uint8_t(t >> 8); h[4] = uint8_t(t);
	uint8_t msg[kCtxLen + 5];
	std::memcpy(msg, kCtx, kCtxLen);
	std::memcpy(msg + kCtxLen, h.data(), 5);
	const auto mac = hmacSha256(key, msg);
	std::memcpy(h.data() + 5, mac.data(), 8);
	return h;
}

bool loadHubKey(const std::string &path, std::vector<uint8_t> &key, std::string &err)
{
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		err = "cannot open hub key file '" + path + "': " + strerror(errno);
		return false;
	}
	struct stat st{};
	if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
	{
		err = "hub key file '" + path + "' is not a regular file";
		::close(fd);
		return false;
	}
	if (st.st_mode & 077)
	{
		err = "hub key file '" + path + "' is accessible by group/other (chmod 600)";
		::close(fd);
		return false;
	}
	std::vector<uint8_t> buf(kHubKeyMax + 1);
	const ssize_t n = ::read(fd, buf.data(), buf.size());
	::close(fd);
	if (n < 0 || size_t(n) > kHubKeyMax)
	{
		err = "hub key file '" + path + (n < 0 ? "' is unreadable" : "' is longer than 1024 bytes");
		return false;
	}
	size_t len = size_t(n);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
	if (len < kHubKeyMin)
	{
		err = "hub key in '" + path + "' is shorter than 16 bytes";
		return false;
	}
	buf.resize(len);
	key = std::move(buf);
	return true;
}
