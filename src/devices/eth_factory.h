/*
	eth_factory — create an EthernetBackend from a --eth spec string.

	Kinds today: "slirp" (needs libslirp) and "none".  New kinds (e.g. a
	hub or tap backend) register in the table in eth_factory.cpp.
*/
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "devices/eth_backend.h"

// `spec` empty means "default": slirp when built in, else none.
// `redirs` are --net-redir specs, applied to backends that support host
// forwards.  Prints a warning to stderr and returns an EthNull on an
// unknown or malformed spec, so the adapter still appears (cable out).
std::unique_ptr<EthernetBackend> CreateEthBackend(const std::string &spec,
												   const std::vector<std::string> &redirs);
