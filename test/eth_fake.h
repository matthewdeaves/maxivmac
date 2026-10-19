// FakeEthernet: an EthernetBackend for unit tests.  Records what the
// adapter sends, queues frames the test injects, link state is settable.
#pragma once

#include <array>
#include <vector>

#include "devices/eth_backend.h"
#include "devices/eth_queue.h"

class FakeEthernet : public EthernetBackend
{
public:
	bool link = true;
	bool acceptSend = true;
	int polls = 0;
	std::vector<std::vector<uint8_t>> sent;

	// Host -> guest.  false if the frame length is out of range.
	bool inject(std::span<const uint8_t> f) { return rx_.push(f); }
	bool inject(const std::vector<uint8_t> &f) { return rx_.push(f); }
	size_t queued() const { return rx_.size(); }
	uint64_t dropped() const { return rx_.dropped(); }
	uint64_t droppedFrames() const override { return rx_.dropped(); }

	bool linkUp() const override { return link; }
	bool send(std::span<const uint8_t> f) override
	{
		if (!link || !acceptSend || f.size() < kEthHeaderLen || f.size() > kEthMaxFrame) return false;
		sent.emplace_back(f.begin(), f.end());
		return true;
	}
	size_t peekSize() const override { return link ? rx_.peekSize() : 0; }
	size_t receive(std::span<uint8_t> dst) override { return link ? rx_.pop(dst) : 0; }
	void poll() override { polls++; }
	const char *name() const override { return "fake"; }

private:
	FrameQueue rx_;
};

// Build a test frame of `len` bytes: dst (default broadcast), src 02:..:99,
// ethertype 0x0800, payload = fill.
inline std::vector<uint8_t> makeFrame(size_t len, uint8_t fill,
									  std::array<uint8_t, 6> dst = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})
{
	std::vector<uint8_t> f(len, fill);
	for (size_t i = 0; i < 6 && i < len; i++) f[i] = dst[i];
	for (size_t i = 6; i < 12 && i < len; i++) f[i] = i == 6 ? 0x02 : i == 11 ? 0x99 : 0x00;
	if (len > 13) { f[12] = 0x08; f[13] = 0x00; }
	return f;
}
