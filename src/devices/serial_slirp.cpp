/*
	serial_slirp.cpp — SLIP networking backend using libslirp.

	Bridges SLIP-framed serial bytes (from the SCC) to/from
	libslirp's IP packet interface.  The libslirp glue lives in
	SlirpStack (slirp_stack.cpp); this file is the SLIP codec plus a
	minimal ARP shim that lets raw IP ride on libslirp's Ethernet.
*/

#if HAVE_SLIRP

#include "devices/serial_slirp.h"
#include <cstdio>
#include <cstring>

/* -----------------------------------------------------------------------
   LOG — NET subsystem (libslirp lifecycle)
   ----------------------------------------------------------------------- */
#include "core/diag.h"
#define NET_LOG(fmt, ...) DIAG(NET, fmt "\n", ##__VA_ARGS__)

SlirpBackend::SlirpBackend()
	: stack_(SlirpStack::Config{}, [this](const uint8_t *p, size_t n) { onSlirpOutput(p, n); })
{
}

SlirpBackend::~SlirpBackend() = default;

/* Ethernet header size */
static constexpr size_t ETH_HLEN = 14;
static constexpr size_t ARP_PKT_LEN = 28; /* ARP payload for IPv4/Ethernet */
static constexpr uint16_t ETHERTYPE_IP = 0x0800;
static constexpr uint16_t ETHERTYPE_ARP = 0x0806;

/* libslirp operates at Layer 2 (Ethernet), but SLIP is Layer 3 (raw IP).
   We maintain a minimal ARP responder so libslirp can resolve our guest's
   MAC before sending IP packets.  The MACs come from the stack's Config. */

/* Read big-endian uint16 from buffer */
static uint16_t readU16BE(const uint8_t *p)
{
	return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void SlirpBackend::txByte(uint8_t byte)
{
	const auto kGuestMAC = stack_.config().guestMac();
	const auto kGatewayMAC = stack_.config().gatewayMac();
	if (decoder_.feed(byte))
	{
		auto pkt = decoder_.packet();
		if (pkt.empty()) return;

		/* Determine EtherType from IP version nibble */
		uint16_t etherType = ETHERTYPE_IP;
		uint8_t ipVer = (pkt[0] >> 4) & 0x0F;
		if (ipVer == 6) etherType = 0x86DD; /* IPv6 */

		/* Wrap raw IP in a fake Ethernet frame for libslirp */
		std::vector<uint8_t> frame;
		frame.reserve(ETH_HLEN + pkt.size());
		frame.insert(frame.end(), kGatewayMAC.begin(), kGatewayMAC.end()); /* dst */
		frame.insert(frame.end(), kGuestMAC.begin(), kGuestMAC.end());	 /* src */
		frame.push_back(static_cast<uint8_t>(etherType >> 8));
		frame.push_back(static_cast<uint8_t>(etherType & 0xFF));
		frame.insert(frame.end(), pkt.begin(), pkt.end());

		SLP_LOG("tx packet: %zu IP bytes -> %zu eth frame -> slirp_input", pkt.size(),
				frame.size());
		stack_.input(frame.data(), frame.size());
	}
}

/* Handle an ARP request from libslirp by auto-replying with our fake MAC.
   This lets libslirp resolve 10.0.2.15 → kGuestMAC so it can deliver
   IP packets addressed to the guest. */
void SlirpBackend::handleArp(const uint8_t *frame, size_t len)
{
	const auto kGuestMAC = stack_.config().guestMac();
	if (len < ETH_HLEN + ARP_PKT_LEN) return;

	const uint8_t *arp = frame + ETH_HLEN;

	uint16_t htype = readU16BE(arp + 0); /* hardware type: 1 = Ethernet */
	uint16_t ptype = readU16BE(arp + 2); /* protocol type: 0x0800 = IPv4 */
	uint16_t oper = readU16BE(arp + 6);	 /* operation: 1 = request */

	if (htype != 1 || ptype != ETHERTYPE_IP || oper != 1)
	{
		NET_LOG("arp: ignoring htype=%u ptype=0x%04X oper=%u", htype, ptype, oper);
		return;
	}

	/* ARP request fields (after htype/ptype/hlen/plen/oper):
	   offset  8: sender MAC (6)
	   offset 14: sender IP  (4)
	   offset 18: target MAC (6) — zeroed in request
	   offset 24: target IP  (4) */
	const uint8_t *senderMAC = arp + 8;
	const uint8_t *senderIP = arp + 14;
	const uint8_t *targetIP = arp + 24;

	NET_LOG("arp: who-has %u.%u.%u.%u? tell %u.%u.%u.%u", targetIP[0], targetIP[1], targetIP[2],
			targetIP[3], senderIP[0], senderIP[1], senderIP[2], senderIP[3]);

	/* Build ARP reply */
	uint8_t reply[ETH_HLEN + ARP_PKT_LEN];
	/* Ethernet header: dst = whoever asked, src = our guest MAC */
	std::memcpy(reply + 0, senderMAC, 6);
	std::memcpy(reply + 6, kGuestMAC.data(), 6);
	reply[12] = 0x08;
	reply[13] = 0x06; /* ARP */

	/* ARP payload */
	uint8_t *r = reply + ETH_HLEN;
	r[0] = 0x00;
	r[1] = 0x01; /* htype = Ethernet */
	r[2] = 0x08;
	r[3] = 0x00; /* ptype = IPv4 */
	r[4] = 6;	 /* hlen */
	r[5] = 4;	 /* plen */
	r[6] = 0x00;
	r[7] = 0x02;					   /* oper = reply */
	std::memcpy(r + 8, kGuestMAC.data(), 6);  /* sender MAC = us */
	std::memcpy(r + 14, targetIP, 4);  /* sender IP = target they asked for */
	std::memcpy(r + 18, senderMAC, 6); /* target MAC = whoever asked */
	std::memcpy(r + 24, senderIP, 4);  /* target IP = their IP */

	NET_LOG("arp: reply %u.%u.%u.%u is-at %02X:%02X:%02X:%02X:%02X:%02X", targetIP[0], targetIP[1],
			targetIP[2], targetIP[3], kGuestMAC[0], kGuestMAC[1], kGuestMAC[2], kGuestMAC[3],
			kGuestMAC[4], kGuestMAC[5]);

	stack_.input(reply, sizeof(reply));
}

void SlirpBackend::onSlirpOutput(const uint8_t *pkt, size_t len)
{
	if (len < ETH_HLEN)
	{
		NET_LOG("rx: runt frame (%zu bytes), dropped", len);
		return;
	}

	uint16_t etherType = readU16BE(pkt + 12);

	if (etherType == ETHERTYPE_ARP)
	{
		/* ARP — handle internally, don't pass to guest SLIP layer */
		handleArp(pkt, len);
		return;
	}

	if (etherType != ETHERTYPE_IP && etherType != 0x86DD)
	{
		NET_LOG("rx: unknown EtherType 0x%04X, dropped", etherType);
		return;
	}

	/* Strip Ethernet header — SLIP carries raw IP only */
	const uint8_t *ip = pkt + ETH_HLEN;
	size_t ipLen = len - ETH_HLEN;

	SLP_LOG("rx packet: %zu eth -> %zu IP bytes -> SLIP encode", len, ipLen);
	std::vector<uint8_t> framed;
	slip::encode(ip, ipLen, framed);
	for (uint8_t b : framed)
		txToGuest_.push(b);
}

bool SlirpBackend::rxReady()
{
	return !txToGuest_.empty();
}

uint8_t SlirpBackend::rxByte()
{
	uint8_t b = txToGuest_.front();
	txToGuest_.pop();
	return b;
}

void SlirpBackend::poll()
{
	stack_.poll();
}

bool SlirpBackend::addHostFwd(bool isUDP, uint16_t hostPort, const char *guestIP,
							  uint16_t guestPort)
{
	return stack_.addHostFwd(isUDP, hostPort, guestIP, guestPort);
}

bool SlirpBackend::addHostFwd(bool isUDP, const char *hostAddr, uint16_t hostPort, const char *guestIP,
							  uint16_t guestPort)
{
	return stack_.addHostFwd(isUDP, hostAddr, hostPort, guestIP, guestPort);
}

#endif /* HAVE_SLIRP */
