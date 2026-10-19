/*
	eth_hub_auth.h — the optional shared-secret hello of the ethhub protocol
	(tools/ethhub/ethhub.c, docs/features/ETHHUB.md "Authentication").

	A keyed hello is a 13-byte datagram (no Ethernet frame is that short, so
	it cannot be mistaken for one):

	  byte 0      0xE7
	  bytes 1..4  unix time in seconds, big endian
	  bytes 5..12 first 8 bytes of HMAC-SHA256(key, "ethhub-hello-v1" || bytes 0..4)

	It replaces the empty hello; frames are unchanged.  This is a join ticket
	for casual-abuse protection, not channel security: see the threat model in
	ETHHUB.md.
*/
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

constexpr size_t kHubHelloLen = 13;
constexpr size_t kHubKeyMin = 16;
constexpr size_t kHubKeyMax = 1024;

std::array<uint8_t, 32> hmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> msg);

// The keyed hello for `unixSeconds` (low 32 bits are sent).
std::array<uint8_t, kHubHelloLen> makeHubHello(std::span<const uint8_t> key, uint64_t unixSeconds);

// Reads a key file the way the hub does: a regular file readable by its owner
// only (no group/other permission bits), trailing CR/LF stripped, 16..1024 bytes.
// Returns false and sets `err` otherwise.
bool loadHubKey(const std::string &path, std::vector<uint8_t> &key, std::string &err);
