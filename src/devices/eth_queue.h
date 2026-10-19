/*
	FrameQueue: the bounded receive queue every EthernetBackend owns.
	Drop-oldest on overflow, with a drop counter.
*/
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "devices/eth_backend.h"

class FrameQueue
{
public:
	explicit FrameQueue(size_t capacity = kEthRxQueueFrames) : cap_(capacity) {}

	// Queue a frame; out-of-range lengths are rejected (not counted as
	// overflow drops).  Returns false when the frame was rejected.
	bool push(std::span<const uint8_t> f)
	{
		if (f.size() < kEthHeaderLen || f.size() > kEthMaxFrame) return false;
		if (q_.size() >= cap_)
		{
			q_.pop_front();
			dropped_++;
		}
		q_.emplace_back(f.begin(), f.end());
		return true;
	}
	size_t peekSize() const { return q_.empty() ? 0 : q_.front().size(); }
	size_t pop(std::span<uint8_t> dst)
	{
		if (q_.empty() || dst.size() < q_.front().size()) return 0;
		const size_t n = q_.front().size();
		std::copy(q_.front().begin(), q_.front().end(), dst.begin());
		q_.pop_front();
		return n;
	}
	void clear() { q_.clear(); }
	size_t size() const { return q_.size(); }
	uint64_t dropped() const { return dropped_; }

private:
	size_t cap_;
	std::deque<std::vector<uint8_t>> q_;
	uint64_t dropped_ = 0;
};
