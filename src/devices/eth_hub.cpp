/*
	eth_hub.cpp — see eth_hub.h.
*/

#include "devices/eth_hub.h"
#include "devices/eth_hub_auth.h"

#include <arpa/inet.h>
#include <cerrno>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

struct EthHub::Peer
{
	sockaddr_in addr{};
};

namespace
{
void secureZero(uint8_t *p, size_t n)
{
	volatile uint8_t *v = p;
	while (n--) *v++ = 0;
}

// Datagrams read per poll(): bounds the work of one tick under a flood.
constexpr int kMaxReadsPerPoll = 128;
} // namespace

std::unique_ptr<EthHub> EthHub::create(const std::string &hostPort, std::string &err, Options opt)
{
	const size_t colon = hostPort.rfind(':');
	if (colon == std::string::npos || colon == 0 || colon + 1 >= hostPort.size())
	{
		err = "--eth=hub needs HOST:PORT (e.g. hub:127.0.0.1:5555), got '" + hostPort + "'";
		return nullptr;
	}
	const std::string host = hostPort.substr(0, colon);
	unsigned port = 0;
	{
		const char *b = hostPort.data() + colon + 1, *e = hostPort.data() + hostPort.size();
		auto [p, ec] = std::from_chars(b, e, port);
		if (ec != std::errc() || p != e || port == 0 || port > 65535)
		{
			err = "bad hub port in '" + hostPort + "' (1..65535)";
			return nullptr;
		}
	}

	addrinfo hints{};
	hints.ai_family = AF_INET; // the hub socket is IPv4 only
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo *res = nullptr;
	const int gai = getaddrinfo(host.c_str(), nullptr, &hints, &res);
	if (gai != 0 || !res)
	{
		err = "cannot resolve hub host '" + host + "': " + gai_strerror(gai);
		return nullptr;
	}
	auto h = std::unique_ptr<EthHub>(new EthHub());
	h->peer_ = std::make_unique<Peer>();
	std::memcpy(&h->peer_->addr, res->ai_addr, sizeof(sockaddr_in));
	freeaddrinfo(res);
	h->peer_->addr.sin_port = htons(static_cast<uint16_t>(port));

	const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
	{
		err = std::string("cannot create UDP socket: ") + strerror(errno);
		return nullptr;
	}
	const int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
	{
		err = std::string("cannot set up UDP socket: ") + strerror(errno);
		::close(fd);
		return nullptr;
	}
	h->fd_ = fd;
	h->opt_ = std::move(opt);
	h->startMs_ = h->now();
	h->hello(); // registers us with the hub (and binds the local port)
	h->startKeepalive();
	return h;
}

EthHub::~EthHub()
{
	if (kaThread_.joinable())
	{
		{
			std::lock_guard<std::mutex> lk(kaMu_);
			kaStop_ = true;
		}
		kaCv_.notify_all();
		kaThread_.join();
	}
	if (!opt_.key.empty()) secureZero(opt_.key.data(), opt_.key.size());
	if (fd_ >= 0) ::close(fd_);
	if (stats_.txDropped || stats_.rxBadSize || stats_.rxForeign || rx_.dropped())
		fprintf(stderr, "[ETH] hub: tx %llu (dropped %llu), rx %llu (queue drops %llu, bad size %llu, foreign %llu)\n",
				(unsigned long long)stats_.txFrames, (unsigned long long)stats_.txDropped,
				(unsigned long long)stats_.rxFrames, (unsigned long long)rx_.dropped(),
				(unsigned long long)stats_.rxBadSize, (unsigned long long)stats_.rxForeign);
}

int64_t EthHub::now() const
{
	if (opt_.wallMs) return opt_.wallMs();
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint16_t EthHub::localPort() const
{
	if (fd_ < 0) return 0;
	sockaddr_in a{};
	socklen_t l = sizeof a;
	if (getsockname(fd_, reinterpret_cast<sockaddr *>(&a), &l) != 0) return 0;
	return ntohs(a.sin_port);
}

bool EthHub::sendDatagram(const uint8_t *p, size_t n)
{
	const ssize_t r = ::sendto(fd_, p, n, 0, reinterpret_cast<const sockaddr *>(&peer_->addr), sizeof peer_->addr);
	return r == static_cast<ssize_t>(n);
}

const EthHub::Stats &EthHub::stats() const
{
	stats_.hellos = helloCount_;
	stats_.keyHint = hinted_;
	return stats_;
}

void EthHub::startKeepalive()
{
	if (!opt_.wallMs && opt_.keepaliveMs > 0) kaThread_ = std::thread([this] { keepaliveLoop(); });
}

void EthHub::keepaliveLoop()
{
	std::unique_lock<std::mutex> lk(kaMu_);
	while (!kaStop_)
	{
		kaCv_.wait_for(lk, std::chrono::milliseconds(opt_.keepaliveMs), [this] { return kaStop_; });
		if (kaStop_) break;
		lk.unlock();
		hello();
		checkKeyHint();
		lk.lock();
	}
}

// One hint, once: keyed hellos have gone out for a while and the hub (which answers each
// accepted one with an empty datagram) never has.  The same silence is what a wrong key, a
// hub without a key and clocks more than --auth-window apart look like.
void EthHub::checkKeyHint()
{
	if (opt_.key.empty() || ackSeen_ || hinted_ || now() - startMs_ < opt_.hintAfterMs) return;
	if (hinted_.exchange(true)) return;
	fprintf(stderr, "[ETH] hub: keyed hellos sent for %lld s and the hub has not answered: check that it runs with the "
					"same --key-file and that both clocks agree to within its --auth-window\n",
			(long long)(opt_.hintAfterMs / 1000));
}

void EthHub::hello()
{
	lastHelloMs_ = now();
	helloCount_++;
	if (!opt_.key.empty())
	{
		const uint64_t t = opt_.unixSec ? opt_.unixSec()
										: uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
													   std::chrono::system_clock::now().time_since_epoch())
													   .count());
		const auto h = makeHubHello(opt_.key, t);
		(void)sendDatagram(h.data(), h.size());
		return;
	}
	(void)sendDatagram(nullptr, 0); // a lost hello is repeated in 5 s
}

bool EthHub::send(std::span<const uint8_t> frame)
{
	if (fd_ < 0 || frame.size() < kEthHeaderLen || frame.size() > kEthMaxFrame) return false;
	if (!sendDatagram(frame.data(), frame.size()))
	{
		// EAGAIN / ENOBUFS: the socket would block.  Anything else (the hub's
		// ICMP "port unreachable" comes back as ECONNREFUSED on some systems)
		// is a drop as well: a LAN frame is lost, never retried.
		stats_.txDropped++;
		return false;
	}
	stats_.txFrames++;
	return true;
}

void EthHub::poll()
{
	if (fd_ < 0) return;
	if (!kaThread_.joinable())
	{
		if (now() - lastHelloMs_ >= opt_.keepaliveMs) hello();
		checkKeyHint();
	}

	uint8_t buf[2048]; // anything larger than a frame arrives truncated and is refused by size
	for (int i = 0; i < kMaxReadsPerPoll; i++)
	{
		sockaddr_in from{};
		socklen_t fl = sizeof from;
		const ssize_t n = ::recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr *>(&from), &fl);
		if (n < 0) break; // EAGAIN: drained; any other error: try again next tick
		if (from.sin_addr.s_addr != peer_->addr.sin_addr.s_addr || from.sin_port != peer_->addr.sin_port)
		{
			stats_.rxForeign++;
			continue;
		}
		if (n == 0)
		{
			ackSeen_ = true; // the hub answered a keyed hello (or echoed a plain one): nothing to deliver
			continue;
		}
		if (n < static_cast<ssize_t>(kEthHeaderLen) || n > static_cast<ssize_t>(kEthMaxFrame))
		{
			stats_.rxBadSize++;
			continue;
		}
		if (rx_.push(std::span<const uint8_t>(buf, static_cast<size_t>(n)))) stats_.rxFrames++;
	}
}
