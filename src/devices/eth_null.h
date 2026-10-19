/*
	eth_null.h — an EthernetBackend with the cable out: link down, every
	frame dropped.  Used for --eth=none and when no real backend exists.
*/
#pragma once

#include "devices/eth_backend.h"

class EthNull : public EthernetBackend
{
public:
	bool linkUp() const override { return false; }
	bool send(std::span<const uint8_t>) override { return false; }
	size_t peekSize() const override { return 0; }
	size_t receive(std::span<uint8_t>) override { return 0; }
	void poll() override {}
	const char *name() const override { return "none"; }
};
