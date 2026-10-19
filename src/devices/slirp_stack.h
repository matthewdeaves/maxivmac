/*
	slirp_stack.h — thin C++ wrapper around libslirp, shared by the SLIP
	serial backend (serial_slirp) and the Ethernet backend (eth_slirp).

	It owns the Slirp instance, the libslirp callbacks, the (minimal) timer
	objects and the non-blocking poll loop.  Frames go in with input() and
	come out through the output callback given at construction.  All the
	addressing lives in one Config so nothing is hard-coded elsewhere.

	Wall-clock vs emulated clock: libslirp runs on the host's steady clock
	(TCP retransmit timers etc.) while poll() is called once per emulated
	tick.  If the emulator runs far faster than real time, the guest sees the
	network as extremely slow and TCP may retransmit aggressively; if it runs
	slower, host-side timers fire "early" relative to the guest.
*/
#pragma once

#if HAVE_SLIRP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <libslirp.h>
#include <string>
#include <vector>

class SlirpStack
{
public:
	/* All addresses are host-order IPv4 values. */
	struct Config
	{
		uint32_t network = 0x0A000200;	 /* 10.0.2.0 */
		uint32_t netmask = 0xFFFFFF00;	 /* 255.255.255.0 */
		uint32_t gateway = 0x0A000202;	 /* 10.0.2.2 */
		uint32_t dns = 0x0A000203;		 /* 10.0.2.3 */
		uint32_t guest = 0x0A00020F;	 /* 10.0.2.15 (DHCP start / SLIP peer) */

		/* libslirp derives its own MAC for an address as 52:55:a.b.c.d. */
		static std::array<uint8_t, 6> macFor(uint32_t ip)
		{
			return {0x52, 0x55, uint8_t(ip >> 24), uint8_t(ip >> 16), uint8_t(ip >> 8),
					uint8_t(ip)};
		}
		std::array<uint8_t, 6> gatewayMac() const { return macFor(gateway); }
		std::array<uint8_t, 6> guestMac() const { return macFor(guest); }
		std::string describe() const;
	};

	using OutputFn = std::function<void(const uint8_t *, size_t)>;

	SlirpStack(const Config &cfg, OutputFn out);
	~SlirpStack();
	SlirpStack(const SlirpStack &) = delete;
	SlirpStack &operator=(const SlirpStack &) = delete;

	bool ok() const { return slirp_ != nullptr; }
	const Config &config() const { return cfg_; }

	/* Guest -> network: one Ethernet frame (no FCS). */
	void input(const uint8_t *frame, size_t len);
	/* Non-blocking pump; call once per emulated tick. */
	void poll();
	/* Host -> guest port forward (TCP or UDP); guestIP "" = config().guest. */
	bool addHostFwd(bool isUDP, uint16_t hostPort, const char *guestIP, uint16_t guestPort);

	/* libslirp callback trampoline. */
	void onOutput(const uint8_t *pkt, size_t len)
	{
		if (out_) out_(pkt, len);
	}

	struct PollEntry
	{
		int fd;
		int events;
		int revents;
	};

private:
	Config cfg_;
	OutputFn out_;
	Slirp *slirp_ = nullptr;
	std::vector<PollEntry> pollFds_;
};

#endif /* HAVE_SLIRP */
