/*
	slirp_stack.cpp — libslirp wrapper shared by serial_slirp and eth_slirp.
	(Factored out of serial_slirp.cpp; libslirp glue is unchanged.)
*/

#if HAVE_SLIRP

#include "devices/slirp_stack.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <poll.h>

#include "core/diag.h"
#define NET_LOG(fmt, ...) DIAG(NET, fmt "\n", ##__VA_ARGS__)

/* -----------------------------------------------------------------------
   libslirp C callbacks
   ----------------------------------------------------------------------- */

static slirp_ssize_t cb_send_packet(const void *buf, size_t len, void *opaque)
{
	auto *self = static_cast<SlirpStack *>(opaque);
	self->onOutput(static_cast<const uint8_t *>(buf), len);
	return static_cast<slirp_ssize_t>(len);
}

static void cb_guest_error(const char *msg, void * /*opaque*/)
{
	(void)msg;
	NET_LOG("guest error: %s", msg);
}

static int64_t cb_clock_get_ns(void * /*opaque*/)
{
	using namespace std::chrono;
	return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

/* Timer support — the v4 timer_new_opaque interface.  libslirp never fires
   its timers itself: they live in a SlirpTimers table and poll() calls
   slirp_handle_timer() for the due ones.  timer_mod takes an expiry in
   milliseconds on the clock that clock_get_ns() reports in nanoseconds. */
static void *cb_timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque)
{
	return static_cast<SlirpStack *>(opaque)->timers().add(int(id), cb_opaque);
}

static void cb_timer_free(void *timer, void *opaque)
{
	static_cast<SlirpStack *>(opaque)->timers().remove(static_cast<SlirpTimers::Timer *>(timer));
}

static void cb_timer_mod(void *timer, int64_t expire_time, void * /*opaque*/)
{
	SlirpTimers::arm(static_cast<SlirpTimers::Timer *>(timer), expire_time);
}

static void cb_register_poll_socket(slirp_os_socket /*fd*/, void * /*opaque*/)
{
	/* nothing — we collect fds during pollfds_fill */
}

static void cb_unregister_poll_socket(slirp_os_socket /*fd*/, void * /*opaque*/)
{
	/* nothing */
}

static void cb_notify(void * /*opaque*/)
{
	/* nothing — we poll synchronously */
}

static void cb_init_completed(Slirp *slirp, void * /*opaque*/)
{
	(void)slirp;
}

/* ------- poll helpers for slirp_pollfds_fill_socket / slirp_pollfds_poll --- */

static int cb_add_poll(slirp_os_socket fd, int events, void *opaque)
{
	auto *fds = static_cast<std::vector<SlirpStack::PollEntry> *>(opaque);
	/* Return the index that get_revents will use. */
	int idx = static_cast<int>(fds->size());
	/* Use a temporary struct to avoid aggregate init issues */
	SlirpStack::PollEntry entry;
	entry.fd = fd;
	entry.events = events;
	entry.revents = 0;
	fds->push_back(entry);
	return idx;
}

static int cb_get_revents(int idx, void *opaque)
{
	auto *fds = static_cast<std::vector<SlirpStack::PollEntry> *>(opaque);
	if (idx < 0 || idx >= static_cast<int>(fds->size())) return 0;
	return (*fds)[static_cast<size_t>(idx)].revents;
}

/* -----------------------------------------------------------------------
   SlirpStack implementation
   ----------------------------------------------------------------------- */

std::string SlirpStack::Config::describe() const
{
	auto ip = [](uint32_t a) {
		char b[16];
		snprintf(b, sizeof b, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
		return std::string(b);
	};
	return "network " + ip(network) + " gateway " + ip(gateway) + " dns " + ip(dns) +
		   " guest " + ip(guest);
}

SlirpStack::SlirpStack(const Config &c, OutputFn out) : cfg_(c), out_(std::move(out))
{
	SlirpConfig cfg;
	std::memset(&cfg, 0, sizeof(cfg));
	cfg.version = 6;
	cfg.restricted = false;
	cfg.in_enabled = true;
	cfg.in6_enabled = false; /* keep it simple — IPv4 only for classic Mac */

	cfg.vnetwork.s_addr = htonl(cfg_.network);
	cfg.vnetmask.s_addr = htonl(cfg_.netmask);
	cfg.vhost.s_addr = htonl(cfg_.gateway);
	cfg.vdhcp_start.s_addr = htonl(cfg_.guest);
	cfg.vnameserver.s_addr = htonl(cfg_.dns);

	static SlirpCb callbacks;
	static bool callbacks_init = false;
	if (!callbacks_init)
	{
		std::memset(&callbacks, 0, sizeof(callbacks));
		callbacks.send_packet = cb_send_packet;
		callbacks.guest_error = cb_guest_error;
		callbacks.clock_get_ns = cb_clock_get_ns;
		callbacks.timer_free = cb_timer_free;
		callbacks.timer_mod = cb_timer_mod;
		callbacks.notify = cb_notify;
		callbacks.init_completed = cb_init_completed;
		callbacks.timer_new_opaque = cb_timer_new_opaque;
		callbacks.register_poll_socket = cb_register_poll_socket;
		callbacks.unregister_poll_socket = cb_unregister_poll_socket;
		callbacks_init = true;
	}

	slirp_ = slirp_new(&cfg, &callbacks, this);
	if (!slirp_)
	{
		NET_LOG("FATAL: slirp_new() failed");
		return;
	}

	NET_LOG("init: %s", cfg_.describe().c_str());
}

SlirpStack::~SlirpStack()
{
	if (slirp_)
	{
		slirp_cleanup(slirp_);
		NET_LOG("shutdown");
	}
}

void SlirpStack::input(const uint8_t *frame, size_t len)
{
	if (slirp_) slirp_input(slirp_, frame, static_cast<int>(len));
}

void SlirpStack::poll()
{
	if (!slirp_) return;

	/* Phase 1: ask libslirp which fds to watch */
	pollFds_.clear();
	uint32_t timeout = UINT32_MAX;
	slirp_pollfds_fill_socket(slirp_, &timeout, cb_add_poll, &pollFds_);

	/* Phase 2: poll (non-blocking) */
	int ret = 0;
	if (!pollFds_.empty())
	{
		/* Build a pollfd array from our entries */
		std::vector<struct pollfd> pfds(pollFds_.size());
		for (size_t i = 0; i < pollFds_.size(); ++i)
		{
			pfds[i].fd = pollFds_[i].fd;
			pfds[i].events = 0;
			if (pollFds_[i].events & SLIRP_POLL_IN) pfds[i].events |= POLLIN;
			if (pollFds_[i].events & SLIRP_POLL_OUT) pfds[i].events |= POLLOUT;
			if (pollFds_[i].events & SLIRP_POLL_PRI) pfds[i].events |= POLLPRI;
			pfds[i].revents = 0;
		}

		ret = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 0);

		/* Convert revents back to SLIRP_POLL_* flags */
		if (ret > 0)
		{
			for (size_t i = 0; i < pfds.size(); ++i)
			{
				int rev = 0;
				if (pfds[i].revents & POLLIN) rev |= SLIRP_POLL_IN;
				if (pfds[i].revents & POLLOUT) rev |= SLIRP_POLL_OUT;
				if (pfds[i].revents & POLLPRI) rev |= SLIRP_POLL_PRI;
				if (pfds[i].revents & POLLERR) rev |= SLIRP_POLL_ERR;
				if (pfds[i].revents & POLLHUP) rev |= SLIRP_POLL_HUP;
				pollFds_[i].revents = rev;
			}
		}
	}

	/* Phase 3: tell libslirp what happened */
	slirp_pollfds_poll(slirp_, (ret < 0) ? 1 : 0, cb_get_revents, &pollFds_);

	/* Phase 4: fire the timers that have come due (retransmit, ARP, ...) */
	timers_.fireDue(cb_clock_get_ns(nullptr) / 1000000, [this](int id, void *cbOpaque) {
		slirp_handle_timer(slirp_, static_cast<SlirpTimerId>(id), cbOpaque);
	});
}

bool SlirpStack::addHostFwd(bool isUDP, const char *hostAddr, uint16_t hostPort, const char *guestIP,
							  uint16_t guestPort)
{
	if (!slirp_) return false;

	/* Loopback unless asked: INADDR_ANY would hand the guest's services to
	   every machine that can reach this host. */
	struct in_addr host_addr;
	if (!hostAddr || !*hostAddr) host_addr.s_addr = htonl(INADDR_LOOPBACK);
	else if (inet_pton(AF_INET, hostAddr, &host_addr) != 1)
	{
		NET_LOG("hostfwd: bad host IP '%s'", hostAddr);
		return false;
	}
	if ((ntohl(host_addr.s_addr) >> 24) != 127)
		fprintf(stderr, "[NET] warning: host forward %s:%u listens on %s, not only on this machine\n",
				isUDP ? "udp" : "tcp", hostPort, hostAddr);

	struct in_addr guest_addr;
	if (!guestIP || !*guestIP) guest_addr.s_addr = htonl(cfg_.guest);
	else if (inet_pton(AF_INET, guestIP, &guest_addr) != 1)
	{
		NET_LOG("hostfwd: bad guest IP '%s'", guestIP);
		return false;
	}

	int rc = slirp_add_hostfwd(slirp_, isUDP ? 1 : 0, host_addr, hostPort, guest_addr, guestPort);
	if (rc < 0)
	{
		NET_LOG("hostfwd: failed %s:%u -> %s:%u", isUDP ? "udp" : "tcp", hostPort, guestIP,
				guestPort);
		return false;
	}

	NET_LOG("hostfwd: %s:%u -> %s:%u", isUDP ? "udp" : "tcp", hostPort, guestIP, guestPort);
	return true;
}

#endif /* HAVE_SLIRP */
