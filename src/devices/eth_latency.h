/*
	eth_latency.h — EthLatency: a decorator that holds every received frame
	back for a minimum time on the *emulated* clock.

	Why: libslirp answers an ICMP echo or an ARP request inside send(), so the
	reply is already queued before the guest's WRITE has even finished.  A real
	LAN never answers within microseconds, and MacTCP (Ping, at least) loses the
	reply when it arrives before the send call has returned: echo #2 and later
	"time out" although every reply was delivered.  Holding frames for ~2 ms of
	guest time makes the order of events what the guest software expects.

	The clock is injected (the scheduler's cycle counter in the emulator, a fake
	in tests), so the behaviour is deterministic for a given emulated run; it is
	not poll counts and not the wall clock.  Frames are time-stamped when the
	decorator first sees them (after every send(), and in poll() / peekSize()),
	and become visible `latency` clock units later.  Order is preserved.

	Latency 0 passes everything straight through.  The decorator keeps the
	EthernetBackend rules: no blocking, bounded queue (drop-oldest, counted).
*/
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "devices/eth_backend.h"

class EthLatency : public EthernetBackend
{
public:
	using Clock = std::function<uint64_t()>; // monotonic emulated time, in any unit

	// `latency` is in the clock's unit.
	EthLatency(std::unique_ptr<EthernetBackend> inner, Clock now, uint64_t latency)
		: inner_(std::move(inner)), now_(std::move(now)), latency_(latency)
	{
	}

	bool linkUp() const override { return inner_->linkUp(); }

	bool send(std::span<const uint8_t> frame) override
	{
		const bool ok = inner_->send(frame);
		absorb(); // a synchronous backend has already queued its answer: stamp it now
		return ok;
	}

	size_t peekSize() const override
	{
		absorb();
		if (!inner_->linkUp() || held_.empty()) return 0;
		return ready(held_.front()) ? held_.front().data.size() : 0;
	}

	size_t receive(std::span<uint8_t> dst) override
	{
		absorb();
		if (!inner_->linkUp() || held_.empty() || !ready(held_.front())) return 0;
		auto &f = held_.front().data;
		if (dst.size() < f.size()) return 0;
		const size_t n = f.size();
		std::copy(f.begin(), f.end(), dst.begin());
		held_.pop_front();
		return n;
	}

	void poll() override
	{
		inner_->poll();
		absorb();
	}

	uint64_t droppedFrames() const override { return inner_->droppedFrames() + dropped_; }
	const char *name() const override { return inner_->name(); }

	uint64_t latency() const { return latency_; }
	size_t held() const { return held_.size(); }
	EthernetBackend &inner() { return *inner_; }

private:
	struct Held
	{
		uint64_t stamp;
		std::vector<uint8_t> data;
	};

	bool ready(const Held &h) const { return now_() - h.stamp >= latency_; }

	// Move frames the inner backend has queued into the hold queue.
	void absorb() const
	{
		while (const size_t n = inner_->peekSize())
		{
			std::vector<uint8_t> f(n);
			if (inner_->receive(f) != n) break;
			if (held_.size() >= kEthRxQueueFrames)
			{
				held_.pop_front();
				dropped_++;
			}
			held_.push_back({now_(), std::move(f)});
		}
	}

	std::unique_ptr<EthernetBackend> inner_;
	Clock now_;
	uint64_t latency_;
	mutable std::deque<Held> held_;
	mutable uint64_t dropped_ = 0;
};
