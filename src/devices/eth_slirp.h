/*
	eth_slirp.h — EthernetBackend on libslirp's user-space NAT.

	Frames go straight into/out of SlirpStack (no SLIP, no ARP shim: the
	guest's own ARP/IP runs over libslirp's virtual Ethernet).  Guest
	10.0.2.15, gateway 10.0.2.2 and DNS 10.0.2.3 by default; see
	SlirpStack::Config.  TCP and UDP host forwards via addHostFwd().

	Clock caveat: libslirp runs on the host clock, poll() once per emulated
	tick; see slirp_stack.h.
*/
#pragma once

#if HAVE_SLIRP

#include "devices/eth_backend.h"
#include "devices/eth_queue.h"
#include "devices/slirp_stack.h"

class EthSlirp : public EthernetBackend
{
public:
	explicit EthSlirp(const SlirpStack::Config &cfg = {});

	bool linkUp() const override { return stack_.ok(); }
	bool send(std::span<const uint8_t> frame) override;
	size_t peekSize() const override { return rx_.peekSize(); }
	size_t receive(std::span<uint8_t> dst) override { return rx_.pop(dst); }
	void poll() override { stack_.poll(); }
	uint64_t droppedFrames() const override { return rx_.dropped(); }
	const char *name() const override { return "slirp"; }

	bool addHostFwd(bool isUDP, uint16_t hostPort, const char *guestIP, uint16_t guestPort)
	{
		return stack_.addHostFwd(isUDP, hostPort, guestIP, guestPort);
	}

private:
	FrameQueue rx_;
	SlirpStack stack_; // after rx_: its callback feeds rx_
};

#endif /* HAVE_SLIRP */
