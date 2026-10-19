/*
	EthernetBackend: the seam between an emulated Ethernet adapter (e.g. the
	DaynaPORT SCSI target) and the host network.

	Rules for implementations:
	  - Called from the emulation thread only; never block.
	  - Start-up may be asynchronous: linkUp() may go false -> true later.
	    Whether an adapter is *present* is the adapter's concern, not the
	    backend's; linkUp() only reports whether frames can currently flow.
	  - The backend owns a bounded receive queue (kEthRxQueueFrames frames).
	    On overflow the oldest frame is dropped and counted (see FrameQueue).
	  - Frames are plain Ethernet II without FCS, 14..1514 bytes.
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

constexpr size_t kEthHeaderLen = 14;
constexpr size_t kEthMaxFrame = 1514;
constexpr size_t kEthRxQueueFrames = 64;

class EthernetBackend
{
public:
	virtual ~EthernetBackend() = default;

	virtual bool linkUp() const = 0;
	// Transmit one frame (14..1514 bytes, no FCS).  false = dropped.
	virtual bool send(std::span<const uint8_t> frame) = 0;
	// Length of the next received frame, 0 = none.  Does not dequeue.
	virtual size_t peekSize() const = 0;
	// Dequeue the next frame into dst; returns its length.
	// Precondition: dst.size() >= peekSize().  Returns 0 if none.
	virtual size_t receive(std::span<uint8_t> dst) = 0;
	// Once per emulated tick; do host I/O here, non-blocking.
	virtual void poll() = 0;
	// Frames dropped by the backend's receive queue (overflow) since start.
	virtual uint64_t droppedFrames() const { return 0; }
	// True when the backend attaches the adapter to a LAN segment that other
	// emulators may share (a hub): the adapter then needs a MAC of its own.
	virtual bool sharesLan() const { return false; }
	virtual const char *name() const = 0;
};
