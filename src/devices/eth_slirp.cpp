/*
	eth_slirp.cpp — EthernetBackend on libslirp.
*/

#if HAVE_SLIRP

#include "devices/eth_slirp.h"

EthSlirp::EthSlirp(const SlirpStack::Config &cfg)
	: stack_(cfg, [this](const uint8_t *p, size_t n) { rx_.push(std::span<const uint8_t>(p, n)); })
{
}

bool EthSlirp::send(std::span<const uint8_t> frame)
{
	if (!stack_.ok() || frame.size() < kEthHeaderLen || frame.size() > kEthMaxFrame) return false;
	stack_.input(frame.data(), frame.size());
	return true;
}

#endif /* HAVE_SLIRP */
