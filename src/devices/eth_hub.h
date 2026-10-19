/*
	eth_hub.h — EthHub: an EthernetBackend that joins an ethhub virtual LAN
	(tools/ethhub, docs/features/ETHHUB.md) over UDP.

	  --eth=hub:HOST:PORT[,key=FILE]

	Wire format (fixed by the hub): one raw Ethernet frame, 14..1514 bytes, per
	UDP datagram in both directions; a zero-length datagram is a hello /
	keepalive.  The hub knows this client by its UDP source address, so the
	socket is bound to one local port for its whole life.

	  - non-blocking socket; nothing here ever waits (frames are dropped and
	    counted when the socket would block);
	  - HOST (IPv4 address or name) is resolved once, when the backend is made;
	  - a hello goes out at start and then every 5 s of WALL time (the hub ages
	    a client out after 60 s without a datagram), whatever the emulated
	    clock does, and even while the emulation is paused: a small timer thread
	    sends them (with an injected wallMs, as the tests do, poll() does);
	  - datagrams from any other source, or of a bad size, are dropped and
	    counted;
	  - received frames go to the usual 64-frame FrameQueue (drop-oldest);
	  - linkUp() is true as soon as the socket is open.  UDP is connectionless:
	    it says nothing about whether a hub is listening or alive.

	With key=FILE (or MAXIVMAC_HUB_KEY_FILE in the environment) the hello is the
	13-byte keyed hello of eth_hub_auth.h, which a hub started with --key-file
	needs before it accepts anything from this socket; frames are unchanged.
	Without a key the wire is byte-for-byte what it always was.

	Several emulators on one hub must have different MACs; sharesLan() tells
	the SCSI factory so it can pick a unique default (see scsi_factory.cpp).

	POSIX only.  Called from the emulation thread.
*/
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "devices/eth_backend.h"
#include "devices/eth_queue.h"

struct EthHubOptions
{
	int64_t keepaliveMs = 5000;
	// Wall clock in milliseconds (monotonic); empty = std::chrono::steady_clock.
	std::function<int64_t()> wallMs;
	// Shared secret for the keyed hello (see eth_hub_auth.h); empty = plain hello.
	std::vector<uint8_t> key;
	// Unix time in seconds for the keyed hello; empty = the system clock.
	std::function<uint64_t()> unixSec;
	// With a key: if the hub has not answered a keyed hello (an empty datagram) this long
	// after start, say so once (wrong key, hub without --key-file, clocks apart).
	int64_t hintAfterMs = 30000;
};

class EthHub : public EthernetBackend
{
public:
	using Options = EthHubOptions;

	struct Stats
	{
		uint64_t txFrames = 0;	  // frames handed to the socket
		uint64_t txDropped = 0;	  // frames dropped: socket would block or failed
		uint64_t rxFrames = 0;	  // frames queued for the guest
		uint64_t rxBadSize = 0;	  // datagrams outside 14..1514 bytes (not counting hellos)
		uint64_t rxForeign = 0;	  // datagrams from a source other than the hub
		uint64_t hellos = 0;	  // hello datagrams sent
		bool keyHint = false;	  // the "no answer to the keyed hello" hint was printed
	};

	// "HOST:PORT" (HOST an IPv4 address or a name, PORT 1..65535).  nullptr and
	// `err` set when the spec is malformed, the name does not resolve or the
	// socket cannot be made.
	static std::unique_ptr<EthHub> create(const std::string &hostPort, std::string &err, Options opt = {});

	~EthHub() override;
	EthHub(const EthHub &) = delete;
	EthHub &operator=(const EthHub &) = delete;

	bool linkUp() const override { return fd_ >= 0; }
	bool send(std::span<const uint8_t> frame) override;
	size_t peekSize() const override { return rx_.peekSize(); }
	size_t receive(std::span<uint8_t> dst) override { return rx_.pop(dst); }
	void poll() override;
	uint64_t droppedFrames() const override { return rx_.dropped(); }
	bool sharesLan() const override { return true; }
	const char *name() const override { return "hub"; }

	const Stats &stats() const;
	// Local UDP port of the socket (0 if closed); the hub sees this as the client.
	uint16_t localPort() const;

private:
	EthHub() = default;
	int64_t now() const;
	bool sendDatagram(const uint8_t *p, size_t n);
	void hello();
	void checkKeyHint();
	void keepaliveLoop();
	void startKeepalive();

	int fd_ = -1;
	// sockaddr_in of the hub, kept opaque here (see the .cpp).
	struct Peer;
	std::unique_ptr<Peer> peer_;
	Options opt_;
	int64_t lastHelloMs_ = 0;	// poll()-driven keepalive (an injected clock); unused with the thread
	int64_t startMs_ = 0;
	FrameQueue rx_;
	mutable Stats stats_;
	std::atomic<uint64_t> helloCount_{0};
	std::atomic<bool> ackSeen_{false}, hinted_{false};
	// Keepalives come from a timer thread on the real clock, not from the 60 Hz emulation
	// tick: a paused emulator (debugger, host sleep) must not look dead to the hub.
	std::thread kaThread_;
	std::mutex kaMu_;
	std::condition_variable kaCv_;
	bool kaStop_ = false;
};
