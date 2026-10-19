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

	static DaynaPortTarget make(FakeEthernet *&out, DaynaPortTarget::Mac mac)
	{
		auto e = std::make_unique<FakeEthernet>();
		out = e.get();
		return DaynaPortTarget(std::move(e), mac);
	}
	explicit DnRig(int scsiId = 4, DaynaPortTarget::Mac mac = DaynaPortTarget::kDefaultMac)
		: t(make(eth, mac)), id(scsiId)
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

const Bytes kSixtyZeros(60, 0);
} // namespace

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
		// 60 zero bytes: known CRC-32 0x04128908.  Ethernet header only needs 14.
		r.eth->inject(Bytes(60, 0));
		Bytes b = r.read(0x08, 1524, dma ? 0xC0 : 0x80, 70, dma);
		CHECK(b[0] == 0);
		CHECK(b[1] == 64);
		CHECK(std::memcmp(b.data() + 66, "\x08\x89\x12\x04", 4) == 0);

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

TEST_CASE("daynaport fix: a READ whose buffer is too small leaves the frame queued")
{
	DnRig r;
	r.enable();
	Bytes f = makeFrame(200, 0x33);
	r.eth->inject(f);
	r.command(0x08, 100, 0xC0); // 100 < 6 + 200 + 4
	r.finish(2);
	CHECK(r.eth->queued() == 1); // donor dequeued and lost it
	Bytes b = r.read(0x08, 1524, 0xC0, 6 + 200 + 4);
	CHECK(Bytes(b.begin() + 6, b.begin() + 206) == f);
	CHECK(r.eth->queued() == 0);
}

TEST_CASE("daynaport fix: READ packs several frames and flags 'more' while frames follow")
{
	DnRig r;
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

TEST_CASE("daynaport fix: frames that do not fit stay queued and the last record says 'more'")
{
	DnRig r;
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

TEST_CASE("daynaport fix: a single queued frame keeps the donor layout (flags zero)")
{
	DnRig r;
	r.enable();
	r.eth->inject(makeFrame(100, 9));
	Bytes b = r.read(0x08, 1524, 0xC0, 6 + 100 + 4);
	CHECK(b[1] == 104);
	CHECK(Bytes(b.begin() + 2, b.begin() + 6) == Bytes(4, 0));
}

TEST_CASE("daynaport fix: link down: TEST UNIT READY succeeds, READ returns no frames, WRITE is dropped")
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

TEST_CASE("daynaport fix: a missing link does not stop selection or INQUIRY")
{
	DnRig r;
	r.eth->link = false;
	CHECK(r.read(0x12, 37, 0, 37)[0] == 3);
	CHECK(r.read(0x09, 6, 0, 18).size() == 18);
}
