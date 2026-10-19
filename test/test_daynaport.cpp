// Tests for the DaynaPORT SCSI/Link target (src/devices/scsi_daynaport.*),
// driven through the real NCR5380 bus model with a FakeEthernet backend.
//
// The first group of cases is ported from the Mini vMac DaynaPORT fork
// (tests/daynaport.c by Mikhail Kandel, GPL-2.0-only), which tested the same
// protocol through its own register-level NCR5380.
#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "devices/scsi_daynaport.h"
#include "eth_fake.h"
#include "scsi_host.h"

namespace
{
using Bytes = std::vector<uint8_t>;

struct DnRig
{
	FakeEthernet *eth;
	DaynaPortTarget t;
	Ncr5380 c;
	Host h{c};
	int id;

	static DaynaPortTarget make(FakeEthernet *&out, DaynaPortTarget::Mac mac, DaynaPortTarget::Options opt)
	{
		auto e = std::make_unique<FakeEthernet>();
		out = e.get();
		return DaynaPortTarget(std::move(e), mac, opt);
	}
	explicit DnRig(int scsiId = 4, DaynaPortTarget::Mac mac = DaynaPortTarget::kDefaultMac,
				   DaynaPortTarget::Options opt = {})
		: t(make(eth, mac, opt)), id(scsiId)
	{
		c.attach(id, &t);
	}

	// Select with ATN, send IDENTIFY, leave the bus in COMMAND phase.
	void connect()
	{
		REQUIRE(h.select(id, true));
		CHECK(h.phase() == P_MSGOUT);
		c.write(ODR, 0xC0, false);
		c.write(ICR, I_DATA, false);
		c.write(ICR, I_DATA | I_ACK, false);
		c.write(ICR, I_DATA, false);
		c.write(ICR, 0, false);
		CHECK(h.phase() == P_CMD);
	}
	// Connect and send a 6-byte CDB laid out as the Dayna driver does.
	void command(uint8_t op, unsigned length, uint8_t flags)
	{
		connect();
		h.sendCdb({op, 0, 0, uint8_t(length >> 8), uint8_t(length), flags});
	}
	Bytes input(size_t n, bool dma)
	{
		Bytes out;
		CHECK(h.phase() == P_DATAIN);
		if (dma)
		{
			c.write(TCR, P_DATAIN, false);
			c.write(MR, 0x02, false);
			c.write(IDMARX, 0, false);
			for (size_t i = 0; i < n; i++)
			{
				CHECK((c.read(BSR, false) & 0x40) != 0);
				out.push_back(c.read(CDR, true));
			}
			CHECK((c.read(BSR, false) & 0x40) == 0);
			CHECK((c.read(BSR, false) & 0x80) != 0); // end of DMA (phase mismatch)
			c.read(RESETR, false);
			c.write(MR, 0, false);
			c.write(TCR, 0, false);
		}
		else
			for (size_t i = 0; i < n; i++) out.push_back(h.recvByte());
		return out;
	}
	void output(const Bytes &d, bool dma)
	{
		CHECK(h.phase() == P_DATAOUT);
		if (dma)
		{
			c.write(TCR, P_DATAOUT, false);
			c.write(MR, 0x02, false);
			c.write(DMATX, 0, false);
			for (uint8_t b : d)
			{
				CHECK((c.read(BSR, false) & 0x40) != 0);
				c.write(ODR, b, true);
			}
			c.write(MR, 0, false);
			c.write(TCR, 0, false);
		}
		else
			for (uint8_t b : d) h.sendByte(b);
	}
	void finish(uint8_t status)
	{
		CHECK(h.phase() == P_STATUS);
		CHECK(h.recvByte() == status);
		CHECK(h.phase() == P_MSGIN);
		CHECK(h.recvByte() == 0);
		CHECK(!h.bsy());
		c.write(TCR, 0, false);
	}
	// A command with a data-in phase, then GOOD status.
	Bytes read(uint8_t op, unsigned length, uint8_t flags, size_t n, bool dma = false)
	{
		command(op, length, flags);
		Bytes b = input(n, dma);
		finish(0);
		return b;
	}
	void enable(bool on = true)
	{
		command(0x0E, 0, on ? 0x80 : 0x00);
		finish(0);
	}
};

// IEEE CRC-32 as sent on the wire (low byte first), for checking the FCS.
uint32_t crc32(const Bytes &d)
{
	uint32_t crc = 0xFFFFFFFFu;
	for (uint8_t b : d)
	{
		crc ^= b;
		for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
	}
	return ~crc;
}
} // namespace

TEST_CASE("daynaport test helper: crc32 matches the standard check value")
{
	CHECK(crc32(Bytes{'1', '2', '3', '4', '5', '6', '7', '8', '9'}) == 0xCBF43926u);
	CHECK(crc32(Bytes(60, 0)) == 0x04128908u); // donor's known vector
}

TEST_CASE("daynaport: selecting an unattached ID never raises BSY")
{
	DnRig r;
	for (int id : {0, 1, 2, 3, 5, 6}) CHECK_FALSE(r.h.select(id, false));
	for (int i = 0; i < 100; i++) CHECK_FALSE(r.h.bsy());
	CHECK(r.h.select(4, false));
}

TEST_CASE("daynaport: INQUIRY, manual and pseudo-DMA")
{
	for (bool dma : {false, true})
	{
		DnRig r;
		Bytes b = r.read(0x12, 37, 0, 37, dma);
		CHECK(b[0] == 3);
		CHECK(b[2] == 1);
		CHECK(b[4] == 31);
		CHECK(std::memcmp(b.data() + 8, "Dayna   SCSI/Link       2.0f", 28) == 0);
	}
}

TEST_CASE("daynaport: INQUIRY honours short and zero allocation lengths")
{
	DnRig r;
	CHECK(r.read(0x12, 5, 0, 5).size() == 5);
	r.command(0x12, 0, 0);
	r.finish(0); // no data phase at all
}

TEST_CASE("daynaport: TEST UNIT READY, MODE SENSE and vendor 0x80 complete GOOD")
{
	DnRig r;
	for (uint8_t op : {0x00, 0x1A, 0x80})
	{
		r.command(op, 0, 0);
		r.finish(0);
	}
}

TEST_CASE("daynaport: GET MAC returns 18 bytes starting with the MAC, even for a size-6 CDB")
{
	DnRig r;
	Bytes b = r.read(0x09, 6, 0, 18);
	CHECK(std::memcmp(b.data(), DaynaPortTarget::kDefaultMac.data(), 6) == 0);
}

TEST_CASE("daynaport: READ with allocation 1 (boot-time probe) fails without a data phase")
{
	DnRig r;
	r.command(0x08, 1, 0);
	r.finish(2);
}

TEST_CASE("daynaport: REQUEST SENSE returns nine bytes and clears the sense")
{
	DnRig r;
	r.command(0x08, 1, 0);
	r.finish(2);
	Bytes b = r.read(0x03, 0, 0, 9);
	CHECK(b[0] == 0x70);
	CHECK(b[2] == 5); // ILLEGAL REQUEST
	b = r.read(0x03, 0, 0, 9);
	CHECK(b[2] == 0);
}

TEST_CASE("daynaport: WRITE while the interface is disabled fails")
{
	DnRig r;
	r.command(0x0A, 60, 0);
	r.finish(2);
}

TEST_CASE("daynaport: READ with nothing queued returns a six-byte zero header")
{
	DnRig r;
	r.enable();
	r.command(0x0C, 0, 0x80); // normal receive mode
	r.finish(0);
	Bytes b = r.read(0x08, 1524, 0xC0, 6);
	CHECK(b == Bytes(6, 0));
}

TEST_CASE("daynaport: READ pads to 60 bytes and appends the FCS; WRITE sends the frame")
{
	for (bool dma : {false, true})
	{
		DnRig r;
		r.enable();
		const Bytes in = makeFrame(60, 0);
		r.eth->inject(in);
		Bytes b = r.read(0x08, 1524, dma ? 0xC0 : 0x80, 70, dma);
		CHECK(b[0] == 0);
		CHECK(b[1] == 64);
		const uint32_t crc = crc32(in);
		const Bytes fcs{uint8_t(crc), uint8_t(crc >> 8), uint8_t(crc >> 16), uint8_t(crc >> 24)};
		CHECK(Bytes(b.begin() + 66, b.begin() + 70) == fcs);

		Bytes frame = makeFrame(60, 0xA5);
		r.command(0x0A, 60, 0);
		r.output(frame, dma);
		r.finish(0);
		REQUIRE(r.eth->sent.size() == 1);
		CHECK(r.eth->sent[0] == frame);
	}
}

TEST_CASE("daynaport: a maximum-size frame is delivered with its FCS")
{
	DnRig r;
	r.enable();
	Bytes f = makeFrame(1514, 0xA5);
	r.eth->inject(f);
	Bytes b = r.read(0x08, 1524, 0xC0, 1524, true);
	CHECK(b[0] == 0x05);
	CHECK(b[1] == 0xEE);
	CHECK(Bytes(b.begin() + 6, b.begin() + 6 + 1514) == f);
}

TEST_CASE("daynaport: header-framed multi-packet WRITE ends at a zero-length record")
{
	for (bool dma : {false, true})
	{
		DnRig r;
		r.enable();
		Bytes rec(132, 0);
		Bytes f = makeFrame(60, 0xA5);
		rec[1] = rec[65] = 60;
		std::copy(f.begin(), f.end(), rec.begin() + 4);
		std::copy(f.begin(), f.end(), rec.begin() + 68);
		r.command(0x0A, 132, 0x80);
		r.output(rec, dma);
		r.finish(0);
		CHECK(r.eth->sent.size() == 2);
	}
}

TEST_CASE("daynaport: bad WRITE sizes and a failing backend give CHECK CONDITION")
{
	DnRig r;
	r.enable();
	r.command(0x0A, 1515, 0);
	r.finish(2);
	r.command(0x0A, 60, 0x80);
	r.output(Bytes{0xFF, 0xFF, 0, 0}, false); // record length 65535
	r.finish(2);
	r.eth->acceptSend = false;
	r.command(0x0A, 60, 0);
	r.output(makeFrame(60, 1), false);
	r.finish(2);
}

TEST_CASE("daynaport: multicast list sizes")
{
	DnRig r;
	r.command(0x0D, 6, 0);
	r.output(Bytes{1, 0, 0x5E, 0, 0, 1}, false);
	r.finish(0);
	r.command(0x0D, 65535, 0);
	r.finish(2);
}

TEST_CASE("daynaport: bus reset aborts a partial transfer and disables the interface")
{
	DnRig r;
	r.enable();
	r.command(0x0A, 60, 0);
	r.h.sendByte(1);
	r.c.write(ICR, 0x80, false); // RST
	r.c.write(ICR, 0, false);
	CHECK_FALSE(r.h.bsy());
	r.command(0x0A, 60, 0);
	r.finish(2); // disabled again
}

TEST_CASE("daynaport: IDENTIFY then command; ABORT message frees the bus")
{
	DnRig r;
	r.command(0x00, 0, 0);
	r.finish(0);
	REQUIRE(r.h.select(4, true));
	r.c.write(ODR, 0x06, false);
	r.c.write(ICR, I_DATA, false);
	r.c.write(ICR, I_DATA | I_ACK, false);
	r.c.write(ICR, 0, false);
	CHECK_FALSE(r.h.bsy());
}

TEST_CASE("daynaport: random register traffic never crashes")
{
	DnRig r;
	uint32_t rnd = 42;
	for (int i = 0; i < 100000; i++)
	{
		rnd = rnd * 1664525u + 1013904223u;
		const uint8_t reg = rnd & 7;
		const bool dack = (rnd >> 3) & 1;
		if ((rnd >> 8) & 1)
			r.c.write(reg, uint8_t(rnd >> 24), dack);
		else
			(void)r.c.read(reg, dack);
		if (i % 1000 == 0) r.c.reset();
	}
}

// ---- fixes over the donor implementation ----

TEST_CASE("daynaport: a frame too big for the READ is dropped, receive goes on")
{
	// The driver never sends REQUEST SENSE and retries with the same length, so
	// a frame that is refused but kept would wedge the receive path forever.
	DnRig r;
	r.enable();
	Bytes f = makeFrame(200, 0x33);
	r.eth->inject(f);
	Bytes h = r.read(0x08, 100, 0xC0, 6); // 100 < 6 + 200 + 4: empty header, GOOD
	CHECK(h == Bytes(6, 0));
	CHECK(r.eth->queued() == 0); // dropped, not stuck at the head of the queue
	// the next frame, which does fit, is delivered by the same READ length
	Bytes g = makeFrame(60, 0x44);
	r.eth->inject(g);
	Bytes b = r.read(0x08, 100, 0xC0, 6 + 60 + 4);
	CHECK(Bytes(b.begin() + 6, b.begin() + 66) == g);
}

TEST_CASE("daynaport: by default a READ returns one frame and flags 'more' while others are queued")
{
	DnRig r;
	r.enable();
	for (uint8_t i = 1; i <= 3; i++) r.eth->inject(makeFrame(60, i));
	for (int i = 0; i < 3; i++)
	{
		Bytes b = r.read(0x08, 1524, 0xC0, 70);
		CHECK(b[5] == (i < 2 ? 0x10 : 0x00));
		CHECK(b[6 + 20] == i + 1);
		CHECK(r.eth->queued() == 2 - i);
	}
}

TEST_CASE("daynaport multiframe: READ packs several frames and flags 'more' while frames follow")
{
	DnRig r(4, DaynaPortTarget::kDefaultMac, {true});
	r.enable();
	for (uint8_t i = 1; i <= 3; i++) r.eth->inject(makeFrame(60, i));
	// each record: 6 header + 60 frame + 4 FCS = 70 bytes
	Bytes b = r.read(0x08, 1524, 0xC0, 3 * 70);
	for (int i = 0; i < 3; i++)
	{
		const Bytes rec(b.begin() + i * 70, b.begin() + (i + 1) * 70);
		CHECK(rec[0] == 0);
		CHECK(rec[1] == 64);
		CHECK(rec[5] == (i < 2 ? 0x10 : 0x00)); // last record: nothing more
		CHECK(rec[6 + 20] == i + 1);
	}
	CHECK(r.eth->queued() == 0);
}

TEST_CASE("daynaport multiframe: frames that do not fit stay queued and the last record says 'more'")
{
	DnRig r(4, DaynaPortTarget::kDefaultMac, {true});
	r.enable();
	for (uint8_t i = 1; i <= 3; i++) r.eth->inject(makeFrame(60, i));
	Bytes b = r.read(0x08, 150, 0xC0, 2 * 70); // room for two records
	CHECK(b[5] == 0x10);
	CHECK(b[70 + 5] == 0x10); // third frame still waiting
	CHECK(r.eth->queued() == 1);
	b = r.read(0x08, 1524, 0xC0, 70);
	CHECK(b[5] == 0x00);
	CHECK(b[6 + 20] == 3);
}

TEST_CASE("daynaport: a single queued frame keeps the donor layout (flags zero)")
{
	DnRig r;
	r.enable();
	r.eth->inject(makeFrame(100, 9));
	Bytes b = r.read(0x08, 1524, 0xC0, 6 + 100 + 4);
	CHECK(b[1] == 104);
	CHECK(Bytes(b.begin() + 2, b.begin() + 6) == Bytes(4, 0));
}

TEST_CASE("daynaport: link down: TEST UNIT READY succeeds, READ returns no frames, WRITE is dropped")
{
	DnRig r;
	r.enable();
	r.eth->link = false;
	r.eth->inject(makeFrame(60, 1)); // hidden while the link is down
	r.command(0x00, 0, 0);
	r.finish(0); // donor: NOT READY
	CHECK(r.read(0x08, 1524, 0xC0, 6) == Bytes(6, 0)); // zero frames, not an error
	r.command(0x0A, 60, 0);
	r.output(makeFrame(60, 2), false);
	r.finish(0);
	CHECK(r.eth->sent.empty());
	r.eth->link = true; // link comes up: the queued frame is still there
	CHECK(r.read(0x08, 1524, 0xC0, 70).size() == 70);
}

TEST_CASE("daynaport: a missing link does not stop selection or INQUIRY")
{
	DnRig r;
	r.eth->link = false;
	CHECK(r.read(0x12, 37, 0, 37)[0] == 3);
	CHECK(r.read(0x09, 6, 0, 18).size() == 18);
}

TEST_CASE("daynaport: works at any SCSI ID 0-6 (donor hard-coded ID 4)")
{
	for (int id = 0; id <= 6; id++)
	{
		DnRig r(id);
		r.enable();
		Bytes b = r.read(0x12, 37, 0, 37);
		CHECK(std::memcmp(b.data() + 8, "Dayna   SCSI/Link", 17) == 0);
		r.eth->inject(makeFrame(60, uint8_t(id)));
		CHECK(r.read(0x08, 1524, 0xC0, 70)[6 + 20] == id);
		for (int other = 0; other <= 6; other++)
			if (other != id) CHECK_FALSE(r.h.select(other, false));
	}
}

TEST_CASE("daynaport: two adapters on one bus are addressed independently")
{
	auto e1 = std::make_unique<FakeEthernet>();
	auto e2 = std::make_unique<FakeEthernet>();
	FakeEthernet *p1 = e1.get(), *p2 = e2.get();
	DaynaPortTarget t1(std::move(e1), {2, 0, 0, 0, 0, 1}), t2(std::move(e2), {2, 0, 0, 0, 0, 2});
	Ncr5380 c;
	Host h{c};
	c.attach(3, &t1);
	c.attach(5, &t2);
	REQUIRE(h.select(5, false));
	h.sendCdb({0x0A, 0, 0, 0, 60, 0}); // WRITE while disabled: CHECK CONDITION from t2 only
	CHECK(h.phase() == P_STATUS);
	CHECK(p1->sent.empty());
	CHECK(p2->sent.empty());
}

TEST_CASE("daynaport: statistics are real and GET MAC reports frames lost")
{
	DnRig r(4, DaynaPortTarget::kDefaultMac, {true});
	r.enable();
	for (int i = 0; i < 70; i++) r.eth->inject(makeFrame(60, uint8_t(i))); // 6 overflow the 64-frame queue
	CHECK(r.read(0x08, 1524, 0xC0, 21 * 70).size() == 21 * 70); // 1524 holds 21 records
	CHECK(r.t.stats().rxFrames == 21);
	CHECK(r.t.rxDropped() == 6);

	Bytes m = r.read(0x09, 6, 0, 18);
	CHECK(std::memcmp(m.data(), DaynaPortTarget::kDefaultMac.data(), 6) == 0);
	CHECK(Bytes(m.begin() + 6, m.begin() + 14) == Bytes(8, 0)); // alignment, CRC errors
	CHECK(Bytes(m.begin() + 14, m.begin() + 18) == Bytes({0, 0, 0, 6})); // frames lost, big-endian

	r.command(0x0A, 60, 0);
	r.output(makeFrame(60, 1), false);
	r.finish(0);
	CHECK(r.t.stats().txFrames == 1);
	r.eth->acceptSend = false;
	r.command(0x0A, 60, 0);
	r.output(makeFrame(60, 1), false);
	r.finish(2);
	r.eth->acceptSend = true;
	r.eth->link = false;
	r.command(0x0A, 60, 0);
	r.output(makeFrame(60, 1), false);
	r.finish(0);
	CHECK(r.t.stats().txFrames == 1);
	CHECK(r.t.stats().txDropped == 2);
}

TEST_CASE("daynaport: a frame dropped for not fitting the READ is counted in rxOversize")
{
	DnRig r;
	r.enable();
	r.eth->inject(makeFrame(200, 1));
	r.read(0x08, 100, 0xC0, 6);
	CHECK(r.t.stats().rxOversize == 1);
	CHECK(r.t.stats().rxFrames == 0);
}

TEST_CASE("daynaport: the adapter's own MAC, broadcast and listed multicast pass; the rest is filtered")
{
	const DaynaPortTarget::Mac mine = DaynaPortTarget::kDefaultMac;
	const DaynaPortTarget::Mac atalk = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF};
	DnRig r(4, DaynaPortTarget::kDefaultMac, {true});
	r.enable();
	r.eth->inject(makeFrame(60, 1, mine));
	r.eth->inject(makeFrame(60, 2, {2, 0, 0, 0, 0, 0x77})); // someone else's unicast
	r.eth->inject(makeFrame(60, 3));						// broadcast
	r.eth->inject(makeFrame(60, 4, atalk));					// multicast, not subscribed
	Bytes b = r.read(0x08, 1524, 0xC0, 2 * 70);
	CHECK(b[6 + 20] == 1);
	CHECK(b[70 + 6 + 20] == 3);
	CHECK(r.t.stats().rxFiltered == 2);
	CHECK(r.eth->queued() == 0);

	r.command(0x0D, 6, 0); // subscribe
	r.output(Bytes(atalk.begin(), atalk.end()), false);
	r.finish(0);
	CHECK(r.t.multicast().size() == 1);
	r.eth->inject(makeFrame(60, 5, atalk));
	CHECK(r.read(0x08, 1524, 0xC0, 70)[6 + 20] == 5);

	r.command(0x0D, 0, 0); // an empty list unsubscribes
	r.finish(0);
	r.eth->inject(makeFrame(60, 6, atalk));
	CHECK(r.read(0x08, 1524, 0xC0, 6) == Bytes(6, 0));
}

TEST_CASE("daynaport: MAC override (0x0C/0x40) is accepted and reported by GET MAC")
{
	DnRig r;
	r.enable();
	const DaynaPortTarget::Mac novel = {2, 0, 0, 0xAB, 0xCD, 0xEF};
	r.command(0x0C, 6, 0x40);
	r.output(Bytes(novel.begin(), novel.end()), false);
	r.finish(0);
	CHECK(r.t.mac() == novel);
	Bytes m = r.read(0x09, 6, 0, 18);
	CHECK(std::memcmp(m.data(), novel.data(), 6) == 0);
	r.eth->inject(makeFrame(60, 1, DaynaPortTarget::kDefaultMac)); // old MAC: no longer ours
	r.eth->inject(makeFrame(60, 2, novel));
	Bytes b = r.read(0x08, 1524, 0xC0, 70);
	CHECK(b[6 + 20] == 2);
	CHECK(r.t.stats().rxFiltered == 1);
}

TEST_CASE("daynaport: MAC override needs six data bytes; a custom --eth-mac is the starting MAC")
{
	DnRig r(4, {2, 1, 2, 3, 4, 5});
	r.command(0x0C, 4, 0x40);
	r.finish(2);
	Bytes m = r.read(0x09, 6, 0, 18);
	CHECK(Bytes(m.begin(), m.begin() + 6) == Bytes{2, 1, 2, 3, 4, 5});
}

TEST_CASE("daynaport: bus reset restores the power-on state")
{
	DnRig r;
	r.enable();
	const DaynaPortTarget::Mac novel = {2, 0, 0, 0xAB, 0xCD, 0xEF};
	r.command(0x0C, 6, 0x40);
	r.output(Bytes(novel.begin(), novel.end()), false);
	r.finish(0);
	r.command(0x0D, 6, 0);
	r.output(Bytes(6, 0x01), false);
	r.finish(0);
	r.command(0x08, 1, 0); // leaves a pending sense
	r.finish(2);
	r.eth->inject(makeFrame(60, 1));
	r.eth->polls = 0;
	r.c.write(ICR, 0x80, false); // RST
	r.c.write(ICR, 0, false);

	CHECK(r.t.mac() == DaynaPortTarget::kDefaultMac);
	CHECK(r.t.multicast().empty());
	CHECK(r.eth->queued() == 0);
	r.command(0x0A, 60, 0); // disabled
	r.finish(2);
	Bytes s = r.read(0x03, 0, 0, 9);
	CHECK(s[2] == 2); // only the NOT READY just above; the old ILLEGAL REQUEST is gone
}

TEST_CASE("daynaport: BUS DEVICE RESET message resets like a bus reset; ABORT keeps state")
{
	DnRig r;
	r.enable();
	auto message = [&](uint8_t m) {
		REQUIRE(r.h.select(4, true));
		r.c.write(ODR, m, false);
		r.c.write(ICR, I_DATA, false);
		r.c.write(ICR, I_DATA | I_ACK, false);
		r.c.write(ICR, 0, false);
		CHECK_FALSE(r.h.bsy());
	};
	message(0x06); // ABORT
	r.command(0x0A, 60, 0);
	r.output(makeFrame(60, 1), false);
	r.finish(0); // still enabled
	message(0x0C); // BUS DEVICE RESET
	r.command(0x0A, 60, 0);
	r.finish(2); // disabled
}

TEST_CASE("daynaport: a bus reset in a record-framed WRITE does not leak into the next command")
{
	DnRig r;
	r.enable();
	r.command(0x0A, 132, 0x80);
	r.h.sendByte(0); // part of a header, then ABORT
	r.c.write(ICR, 0x80, false);
	r.c.write(ICR, 0, false);
	r.enable();
	r.command(0x0A, 60, 0);
	r.output(makeFrame(60, 1), false);
	r.finish(0);
	CHECK(r.eth->sent.size() == 1);
}

TEST_CASE("daynaport: tick() pumps the backend once per call")
{
	DnRig r;
	r.t.tick();
	r.t.tick();
	CHECK(r.eth->polls == 2);
}

TEST_CASE("daynaport: frames of a READ cut short by a bus reset are counted in rxLost")
{
	DnRig r;
	r.enable();
	r.eth->inject(makeFrame(60, 1));
	r.command(0x08, 1524, 0xC0);
	CHECK(r.h.recvByte() == 0); // part of the record only
	r.c.write(ICR, 0x80, false); // RST
	r.c.write(ICR, 0, false);
	CHECK(r.t.stats().rxFrames == 1);
	CHECK(r.t.stats().rxLost == 1);
}

TEST_CASE("daynaport: frames of a READ cut short by an ABORT message are counted in rxLost")
{
	DnRig r(4, DaynaPortTarget::kDefaultMac, {true});
	r.enable();
	for (uint8_t i = 1; i <= 2; i++) r.eth->inject(makeFrame(60, i));
	r.command(0x08, 1524, 0xC0);
	CHECK(r.h.recvByte() == 0);
	r.c.write(ICR, I_ATN | I_ACK, false); // ATN with the next byte: MESSAGE OUT follows
	r.c.write(ICR, I_ATN, false);
	CHECK(r.h.phase() == P_MSGOUT);
	r.c.write(ODR, 0x06, false); // ABORT
	r.c.write(ICR, I_DATA, false);
	r.c.write(ICR, I_DATA | I_ACK, false);
	r.c.write(ICR, 0, false);
	CHECK_FALSE(r.h.bsy());
	CHECK(r.t.stats().rxLost == 2);
}

TEST_CASE("daynaport: a completed READ loses nothing")
{
	DnRig r;
	r.enable();
	r.eth->inject(makeFrame(60, 1));
	r.read(0x08, 1524, 0xC0, 70);
	r.read(0x08, 1524, 0xC0, 6);
	CHECK(r.t.stats().rxLost == 0);
}

TEST_CASE("daynaport: sense data is cleared by the next command but not by INQUIRY")
{
	DnRig r;
	r.command(0x0A, 60, 0); // WRITE while disabled: NOT READY
	r.finish(2);
	r.read(0x12, 36, 0, 36); // INQUIRY keeps it
	Bytes s1 = r.read(0x03, 9, 0, 9);
	CHECK(s1[2] == 2);
	Bytes s2 = r.read(0x03, 9, 0, 9);
	CHECK(s2[2] == 0);
	r.command(0x0A, 60, 0);
	r.finish(2);
	r.command(0x00, 0, 0); // TEST UNIT READY clears it
	r.finish(0);
	CHECK(r.read(0x03, 9, 0, 9)[2] == 0);
}
