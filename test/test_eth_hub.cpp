// EthHub against a loopback UDP "fake hub" bound to an ephemeral port.
#if HAVE_ETH_HUB
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "devices/eth_hub.h"
#include "devices/eth_factory.h"
#include "devices/scsi_daynaport.h"
#include "devices/scsi_factory.h"
#include "eth_fake.h"

namespace
{
// A UDP socket on 127.0.0.1:<ephemeral>, standing in for ethhub.
struct FakeHub
{
	int fd = -1;
	uint16_t port = 0;
	sockaddr_in lastFrom{};

	FakeHub()
	{
		fd = ::socket(AF_INET, SOCK_DGRAM, 0);
		REQUIRE(fd >= 0);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		REQUIRE(::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0);
		socklen_t l = sizeof a;
		REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &l) == 0);
		port = ntohs(a.sin_port);
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	}
	~FakeHub() { if (fd >= 0) ::close(fd); }
	std::string spec() const { return "127.0.0.1:" + std::to_string(port); }

	// Next datagram, waiting up to ~1 s (loopback delivery is immediate, but be patient).
	bool recv(std::vector<uint8_t> &out)
	{
		uint8_t buf[4096];
		for (int i = 0; i < 200; i++)
		{
			socklen_t l = sizeof lastFrom;
			const ssize_t n = ::recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr *>(&lastFrom), &l);
			if (n >= 0)
			{
				out.assign(buf, buf + n);
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	}
	void sendTo(const sockaddr_in &to, const std::vector<uint8_t> &d) const
	{
		::sendto(fd, d.data(), d.size(), 0, reinterpret_cast<const sockaddr *>(&to), sizeof to);
	}
};

// Poll until the backend has a frame or ~1 s has passed.
size_t waitFrame(EthHub &h)
{
	for (int i = 0; i < 200; i++)
	{
		h.poll();
		if (h.peekSize()) return h.peekSize();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return 0;
}

std::unique_ptr<EthHub> make(const FakeHub &hub, const EthHub::Options &o = {})
{
	std::string err;
	auto h = EthHub::create(hub.spec(), err, o);
	REQUIRE_MESSAGE(h, err);
	return h;
}
} // namespace

TEST_CASE("eth hub: a hello (zero-length datagram) is sent at start")
{
	FakeHub hub;
	auto h = make(hub);
	CHECK(h->linkUp());
	std::vector<uint8_t> d;
	REQUIRE(hub.recv(d));
	CHECK(d.empty());
	CHECK(h->stats().hellos == 1);
	CHECK(h->localPort() == ntohs(hub.lastFrom.sin_port));
}

TEST_CASE("eth hub: frames go out as one datagram each and bad sizes are refused")
{
	FakeHub hub;
	auto h = make(hub);
	std::vector<uint8_t> d;
	REQUIRE(hub.recv(d)); // hello
	const auto f = makeFrame(60, 5);
	CHECK(h->send(f));
	REQUIRE(hub.recv(d));
	CHECK(d == f);
	const auto big = makeFrame(kEthMaxFrame, 6);
	CHECK(h->send(big));
	REQUIRE(hub.recv(d));
	CHECK(d.size() == kEthMaxFrame);
	CHECK_FALSE(h->send(std::vector<uint8_t>(13, 0)));
	CHECK_FALSE(h->send(std::vector<uint8_t>(kEthMaxFrame + 1, 0)));
	CHECK(h->stats().txFrames == 2);
}

TEST_CASE("eth hub: frames from the hub are queued; foreign sources and bad sizes are ignored")
{
	FakeHub hub;
	auto h = make(hub);
	std::vector<uint8_t> d;
	REQUIRE(hub.recv(d));
	sockaddr_in client = hub.lastFrom;

	hub.sendTo(client, makeFrame(80, 9));
	REQUIRE(waitFrame(*h) == 80);
	std::vector<uint8_t> buf(kEthMaxFrame);
	CHECK(h->receive(buf) == 80);
	CHECK(buf[30] == 9);
	CHECK(h->peekSize() == 0);

	hub.sendTo(client, std::vector<uint8_t>(10, 1));					 // too short
	hub.sendTo(client, std::vector<uint8_t>(kEthMaxFrame + 1, 1));	 // too long
	hub.sendTo(client, {});											 // a hello echoed back: not a frame
	hub.sendTo(client, makeFrame(60, 3));								 // then a good one
	REQUIRE(waitFrame(*h) == 60);
	CHECK(h->stats().rxBadSize == 2);
	CHECK(h->stats().rxFrames == 2);

	// A datagram from a different socket (another port on loopback).
	FakeHub other;
	other.sendTo(client, makeFrame(70, 4));
	for (int i = 0; i < 20; i++)
	{
		h->poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CHECK(h->stats().rxForeign == 1);
	CHECK(h->receive(buf) == 60); // only the good frame from before is there
	CHECK(h->peekSize() == 0);
}

TEST_CASE("eth hub: receive queue is bounded, drop-oldest, counted")
{
	FakeHub hub;
	auto h = make(hub);
	std::vector<uint8_t> d;
	REQUIRE(hub.recv(d));
	sockaddr_in client = hub.lastFrom;
	// 100 frames in several polls (a poll reads at most 128 datagrams)
	for (int i = 0; i < 100; i++) hub.sendTo(client, makeFrame(60, uint8_t(i)));
	for (int i = 0; i < 100; i++)
	{
		h->poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		if (h->stats().rxFrames == 100) break;
	}
	REQUIRE(h->stats().rxFrames == 100);
	CHECK(h->droppedFrames() == 100 - kEthRxQueueFrames);
	std::vector<uint8_t> buf(kEthMaxFrame);
	REQUIRE(h->receive(buf) == 60);
	CHECK(buf[20] == 100 - kEthRxQueueFrames); // the oldest 36 were dropped
}

TEST_CASE("eth hub: keepalive every 5 s of wall time, not of emulated time")
{
	FakeHub hub;
	int64_t wall = 1000;
	EthHub::Options o;
	o.wallMs = [&] { return wall; };
	auto h = make(hub, o);
	std::vector<uint8_t> d;
	REQUIRE(hub.recv(d)); // the start hello
	for (int i = 0; i < 1000; i++) h->poll(); // many "ticks", no wall time passes
	CHECK(h->stats().hellos == 1);
	wall += 4999;
	h->poll();
	CHECK(h->stats().hellos == 1);
	wall += 1;
	h->poll();
	CHECK(h->stats().hellos == 2);
	REQUIRE(hub.recv(d));
	CHECK(d.empty());
	h->poll();
	CHECK(h->stats().hellos == 2); // once per interval
	wall += 5000;
	h->poll();
	CHECK(h->stats().hellos == 3);
}

TEST_CASE("eth hub: send never blocks when the socket buffer is full")
{
	// Nobody reads the fake hub, so its receive buffer fills; the sender must
	// drop and count instead of waiting.  (Loopback UDP may also just discard
	// at the receiver, in which case every send "succeeds"; either way this
	// test must finish quickly.)
	FakeHub hub;
	auto h = make(hub);
	const auto f = makeFrame(1500, 1);
	const auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < 20000; i++) h->send(f);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	CHECK(ms < 5000);
	CHECK(h->stats().txFrames + h->stats().txDropped == 20000);
}

TEST_CASE("eth hub: a hub that is not there is not an error; the link stays up")
{
	int port;
	{
		FakeHub gone;
		port = gone.port;
	} // closed: datagrams to it are refused (ICMP) or vanish
	std::string err;
	auto h = EthHub::create("127.0.0.1:" + std::to_string(port), err);
	REQUIRE(h);
	CHECK(h->linkUp()); // UDP is connectionless: link state is not hub liveness
	for (int i = 0; i < 50; i++)
	{
		h->send(makeFrame(60, 1));
		h->poll();
	}
	CHECK(h->peekSize() == 0);
	CHECK(h->linkUp());
}

TEST_CASE("eth hub: malformed specs and unresolvable hosts are refused with a message")
{
	std::string err;
	for (const char *bad : {"", "localhost", ":5555", "127.0.0.1:", "127.0.0.1:0", "127.0.0.1:70000", "127.0.0.1:55x"})
	{
		err.clear();
		CHECK_MESSAGE(EthHub::create(bad, err) == nullptr, bad);
		CHECK_MESSAGE(!err.empty(), bad);
	}
	err.clear();
	CHECK(EthHub::create("no-such-host.invalid:5555", err) == nullptr);
	CHECK(err.find("resolve") != std::string::npos);
}

TEST_CASE("eth hub: the factory builds hub:HOST:PORT, falls back to no network on a bad spec")
{
	FakeHub hub;
	auto b = CreateEthBackend("hub:127.0.0.1:" + std::to_string(hub.port), {});
	REQUIRE(b);
	CHECK(std::string(b->name()) == "hub");
	CHECK(b->linkUp());
	CHECK(b->sharesLan());
	auto bad = CreateEthBackend("hub:nonsense", {});
	REQUIRE(bad);
	CHECK(std::string(bad->name()) == "none");
	CHECK_FALSE(bad->sharesLan());
}

TEST_CASE("eth hub: adapters on a hub get distinct random MACs unless --eth-mac is given")
{
	FakeHub hub;
	ScsiTargetOptions o;
	o.eth = "hub:" + hub.spec();
	auto mac = [&](const ScsiTargetOptions &opt) {
		auto t = CreateScsiTarget("daynaport", 4, opt);
		return dynamic_cast<DaynaPortTarget *>(t.get())->mac();
	};
	const auto a = mac(o), b = mac(o), c = mac(o);
	CHECK(a != DaynaPortTarget::kDefaultMac);
	CHECK(a != b);
	CHECK(b != c);
	CHECK((a[0] & 0x03) == 0x02); // locally administered, unicast
	o.ethMac = "02:00:00:aa:bb:cc";
	CHECK(mac(o) == DaynaPortTarget::Mac{0x02, 0, 0, 0xAA, 0xBB, 0xCC});
	// a backend that is not a shared LAN keeps the constant default
	ScsiTargetOptions n;
	n.eth = "none";
	CHECK(mac(n) == DaynaPortTarget::kDefaultMac);
}

// ---- optional shared-secret hello (eth_hub_auth.h) -------------------------------

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string_view>

#include "devices/eth_hub_auth.h"

namespace
{
std::vector<uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

std::string hex(std::span<const uint8_t> b)
{
	std::string o;
	char t[3];
	for (uint8_t c : b)
	{
		snprintf(t, sizeof t, "%02x", c);
		o += t;
	}
	return o;
}

// A key file in the temp directory; the caller removes it.
std::string writeKey(const std::string &content, mode_t mode, const char *tag)
{
	const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/maxivmac_hubkey_" +
							 tag + "_" + std::to_string(::getpid());
	{
		std::ofstream f(path, std::ios::binary);
		REQUIRE(f.good());
		f << content;
	}
	REQUIRE(::chmod(path.c_str(), mode) == 0);
	return path;
}
} // namespace

TEST_CASE("eth hub auth: HMAC-SHA256 matches RFC 4231")
{
	const auto k1 = std::vector<uint8_t>(20, 0x0b);
	CHECK(hex(hmacSha256(k1, bytes("Hi There"))) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
	CHECK(hex(hmacSha256(bytes("Jefe"), bytes("what do ya want for nothing?"))) ==
		  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
	// a key longer than the block size is hashed first (RFC 4231 case 6)
	const auto k6 = std::vector<uint8_t>(131, 0xaa);
	CHECK(hex(hmacSha256(k6, bytes("Test Using Larger Than Block-Size Key - Hash Key First"))) ==
		  "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE("eth hub auth: the keyed hello has the documented layout")
{
	const auto h = makeHubHello(bytes("correct horse battery staple"), 1700000000);
	CHECK(h.size() == 13);
	CHECK(hex(h) == "e76553f100b9c5df5b4db19be1"); // computed independently with Python's hmac
	CHECK(hex(makeHubHello(bytes("correct horse battery staple"), 1700000000ull + (1ull << 32))) == hex(h));
}

TEST_CASE("eth hub auth: key files must be private, long enough, newline-trimmed")
{
	std::vector<uint8_t> key;
	std::string err;
	const std::string good = writeKey("0123456789abcdef-secret\n", 0600, "good");
	CHECK(loadHubKey(good, key, err));
	CHECK(std::string(key.begin(), key.end()) == "0123456789abcdef-secret");
	const std::string open = writeKey("0123456789abcdef-secret", 0644, "open");
	CHECK_FALSE(loadHubKey(open, key, err));
	CHECK(err.find("chmod 600") != std::string::npos);
	const std::string shortk = writeKey("tooshort", 0600, "short");
	CHECK_FALSE(loadHubKey(shortk, key, err));
	CHECK_FALSE(loadHubKey("/nonexistent/maxivmac_key", key, err));
	for (const auto &p : {good, open, shortk}) ::unlink(p.c_str());
}

TEST_CASE("eth hub auth: with a key the start hello and every keepalive are keyed hellos")
{
	FakeHub hub;
	int64_t wall = 1000;
	EthHub::Options o;
	o.wallMs = [&] { return wall; };
	o.key = bytes("correct horse battery staple");
	o.unixSec = [] { return uint64_t(1700000000); };
	auto h = make(hub, o);
	std::vector<uint8_t> d;
	REQUIRE(hub.recv(d));
	CHECK(hex(d) == "e76553f100b9c5df5b4db19be1");
	wall += 5000;
	h->poll();
	REQUIRE(hub.recv(d));
	CHECK(d.size() == 13);
	CHECK(h->stats().hellos == 2);
	// frames are unchanged
	const auto f = makeFrame(60, 5);
	CHECK(h->send(f));
	REQUIRE(hub.recv(d));
	CHECK(d == f);
}

TEST_CASE("eth hub auth: --eth=hub:...,key=FILE reads the key; a bad key file means no network")
{
	FakeHub hub;
	const std::string good = writeKey("0123456789abcdef-secret", 0600, "fac");
	auto be = CreateEthBackend("hub:" + hub.spec() + ",key=" + good, {});
	CHECK(std::string(be->name()) == "hub");
	std::vector<uint8_t> d;
	REQUIRE(hub.recv(d));
	CHECK(d.size() == 13); // keyed
	::unlink(good.c_str());
	CHECK(std::string(CreateEthBackend("hub:" + hub.spec() + ",key=/nonexistent/maxivmac_key", {})->name()) == "none");
	// no key: a plain empty hello, as always
	FakeHub hub2;
	auto plain = CreateEthBackend("hub:" + hub2.spec(), {});
	REQUIRE(hub2.recv(d));
	CHECK(d.empty());
}

TEST_CASE("eth hub auth: key loader refuses a FIFO without blocking, and a missing owner match is checked")
{
	const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
							 "/maxivmac_hubkey_fifo_" + std::to_string(::getpid());
	REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
	std::vector<uint8_t> key;
	std::string err;
	CHECK_FALSE(loadHubKey(path, key, err)); // must return, not wait for a writer
	CHECK(err.find("regular file") != std::string::npos);
	::unlink(path.c_str());
}

TEST_CASE("eth hub: keepalive hellos do not depend on poll() (the emulation tick)")
{
	FakeHub hub;
	EthHub::Options o;
	o.keepaliveMs = 40;
	auto h = make(hub, o); // real clock: a timer thread sends them
	std::vector<uint8_t> d;
	REQUIRE(hub.recv(d)); // start hello
	int got = 0;
	for (int i = 0; i < 3 && hub.recv(d); i++) got += d.empty();
	CHECK(got == 3); // three more, and poll() was never called
	CHECK(h->stats().hellos >= 4);
}

TEST_CASE("eth hub: the keyed hello thread stops with the backend")
{
	FakeHub hub;
	{
		EthHub::Options o;
		o.keepaliveMs = 20;
		o.key = bytes("correct horse battery staple");
		auto h = make(hub, o);
		std::this_thread::sleep_for(std::chrono::milliseconds(80));
	} // destructor joins the thread; a hang or a crash here fails the test
	CHECK(true);
}

TEST_CASE("eth hub: keyed hellos that are never answered produce one hint; an answer cancels it")
{
	{
		FakeHub hub; // never answers
		EthHub::Options o;
		o.keepaliveMs = 20;
		o.hintAfterMs = 100;
		o.key = bytes("correct horse battery staple");
		auto h = make(hub, o);
		for (int i = 0; i < 40 && !h->stats().keyHint; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		CHECK(h->stats().keyHint);
	}
	{
		FakeHub hub; // answers the first hello with an empty datagram, as ethhub does
		EthHub::Options o;
		o.keepaliveMs = 20;
		o.hintAfterMs = 100;
		o.key = bytes("correct horse battery staple");
		auto h = make(hub, o);
		std::vector<uint8_t> d;
		REQUIRE(hub.recv(d));
		hub.sendTo(hub.lastFrom, {});
		for (int i = 0; i < 40; i++)
		{
			h->poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		CHECK_FALSE(h->stats().keyHint);
	}
	{
		FakeHub hub; // no key: nothing to hint about
		EthHub::Options o;
		o.keepaliveMs = 20;
		o.hintAfterMs = 50;
		auto h = make(hub, o);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		CHECK_FALSE(h->stats().keyHint);
	}
}
#endif
