// Tests for the NCR5380 model (src/devices/ncr5380.*).
#include <doctest/doctest.h>

#include "devices/ncr5380.h"
#include "devices/scsi_addr.h"
#include "scsi_scripted_target.h"

#include <memory>

namespace
{
// Register numbers (A0-A2).
enum : uint8_t
{
	CDR = 0, ODR = 0, ICR = 1, MR = 2, TCR = 3, CSR = 4, SER = 4, BSR = 5, IDR = 6,
	RESETR = 7, DMATX = 5, TDMARX = 6, IDMARX = 7
};

// ICR bits
constexpr uint8_t I_ACK = 0x10, I_BSY = 0x08, I_SEL = 0x04, I_ATN = 0x02, I_DATA = 0x01;
// CSBS bits
constexpr uint8_t C_BSY = 0x40, C_REQ = 0x20;
// phases as MSG<<2|CD<<1|IO
constexpr uint8_t P_DATAOUT = 0, P_DATAIN = 1, P_CMD = 2, P_STATUS = 3, P_MSGOUT = 6, P_MSGIN = 7;

// Drives an Ncr5380 the way a SCSI Manager driver does.
struct Host
{
	Ncr5380 &c;
	uint8_t phase() { return (c.read(CSR, false) >> 2) & 7; }
	bool req() { return (c.read(CSR, false) & C_REQ) != 0; }
	bool bsy() { return (c.read(CSR, false) & C_BSY) != 0; }

	// Arbitrate and select `id`; returns true when the target raised BSY.
	bool select(int id, bool atn)
	{
		c.write(ODR, 0x80, false);
		c.write(MR, 0x01, false);
		if (!(c.read(ICR, false) & 0x40)) return false; // AIP
		if (c.read(ICR, false) & 0x20) return false;	 // LA
		c.write(ICR, I_SEL, false);
		c.write(ODR, uint8_t(0x80 | (1 << id)), false);
		c.write(MR, 0x00, false);
		const uint8_t a = atn ? I_ATN : 0;
		c.write(ICR, I_DATA | I_BSY | I_SEL | a, false);
		c.write(ICR, I_DATA | I_SEL | a, false);
		for (int i = 0; i < 50; i++)
			if (bsy())
			{
				c.write(ICR, a, false); // release SEL and data bus
				return true;
			}
		c.write(ICR, 0, false);
		return false;
	}
	// Send one byte with a manual REQ/ACK handshake.
	void sendByte(uint8_t b, uint8_t keep = 0)
	{
		CHECK(req());
		c.write(ODR, b, false);
		c.write(ICR, I_DATA | I_ACK | keep, false);
		CHECK(!req());
		c.write(ICR, I_DATA | keep, false);
	}
	uint8_t recvByte()
	{
		CHECK(req());
		const uint8_t b = c.read(CDR, false);
		c.write(ICR, I_ACK, false);
		CHECK(!req());
		c.write(ICR, 0, false);
		return b;
	}
	void identify(uint8_t id = 0xC0) { sendByte(id, 0); } // ATN already dropped by caller
	void sendCdb(std::initializer_list<uint8_t> cdb)
	{
		for (uint8_t b : cdb) sendByte(b);
	}
	void finish()
	{
		CHECK(phase() == P_STATUS);
		CHECK(recvByte() == 0x00);
		CHECK(phase() == P_MSGIN);
		CHECK(recvByte() == 0x00);
		CHECK(!bsy());
		c.write(TCR, 0, false);
	}
};

struct Rig
{
	ScriptedTarget t;
	Ncr5380 c;
	Host h{c};
	explicit Rig(int id = 4) { c.attach(id, &t); }
	// select with ATN, send IDENTIFY (ATN dropped with the ACK), expect COMMAND phase
	void connect(int id = 4)
	{
		REQUIRE(h.select(id, true));
		CHECK(h.phase() == P_MSGOUT);
		h.c.write(ODR, 0xC0, false);
		h.c.write(ICR, I_DATA, false);		 // drop ATN
		h.c.write(ICR, I_DATA | I_ACK, false);
		h.c.write(ICR, I_DATA, false);
		h.c.write(ICR, 0, false);
		CHECK(h.phase() == P_CMD);
	}
};
} // namespace

TEST_CASE("ncr5380 legacy: all registers read zero after reset()")
{
	Ncr5380 c;
	c.reset();
	for (uint8_t r = 0; r < 8; r++) CHECK(c.read(r, false) == 0);
}

TEST_CASE("ncr5380 legacy: write and read banks are separate")
{
	Ncr5380 c;
	c.reset();
	c.write(TCR, 0x05, false);
	CHECK(c.read(TCR, false) == 0); // read side is untouched by writes
	c.write(MR, 0x40, false);
	CHECK(c.read(MR, false) == 0);
}

TEST_CASE("ncr5380 legacy: RST assertion gives post-reset register values and fires hook")
{
	int resets = 0;
	Ncr5380::Lines l;
	l.onBusReset = [&] { resets++; };
	Ncr5380 c(l);
	c.reset();
	c.write(ICR, 0x80, false); // assert RST
	CHECK(resets == 1);
	CHECK(c.read(CDR, false) == 0x00);
	CHECK(c.read(ICR, false) == 0x80);
	CHECK(c.read(MR, false) == 0x00);
	CHECK(c.read(TCR, false) == 0x00);
	CHECK(c.read(CSR, false) == 0x80);
	CHECK(c.read(BSR, false) == 0x10);
	CHECK(c.read(IDR, false) == 0x00);
	CHECK(c.read(RESETR, false) == 0x00);
	// Release RST: the RST status bits clear.
	c.write(ICR, 0x00, false);
	CHECK(c.read(ICR, false) == 0x00);
	CHECK(c.read(CSR, false) == 0x00);
}

TEST_CASE("ncr5380 legacy: arbitration stub reports AIP and not LA")
{
	Ncr5380 c;
	c.reset();
	c.write(ODR, 0x80, false);
	c.write(MR, 0x01, false); // ARBITRATE
	CHECK((c.read(ICR, false) & 0x60) == 0x40);
	CHECK(c.read(CDR, false) == 0x00);
}

TEST_CASE("ncr5380 legacy: SEL assertion quirk only triggers when ICR == SEL")
{
	Ncr5380 c;
	c.reset();
	c.write(ICR, 0x04, false);
	CHECK(c.read(CSR, false) == 0x02);
	CHECK(c.read(BSR, false) == 0x10);
	c.write(ICR, 0x00, false);
	CHECK(c.read(CSR, false) == 0x00);
}

TEST_CASE("ncr5380 legacy: selecting any ID never raises BSY")
{
	Ncr5380 c;
	c.reset();
	c.write(ODR, 0x90, false);
	c.write(MR, 0x01, false);
	c.write(ICR, 0x04 | 0x08, false);
	for (int i = 0; i < 100; i++) CHECK((c.read(CSR, false) & 0x40) == 0);
}

TEST_CASE("scsi_addr: register comes from A4-A6")
{
	for (uint32_t r = 0; r < 8; r++)
	{
		CHECK(scsi_addr::decode(0x580000 | (r << 4), false) == r);
		CHECK(scsi_addr::decode(0x580001 | (r << 4), false) == r);
	}
}

TEST_CASE("scsi_addr: A9 is /DACK on compact Macs, ignored on II family")
{
	CHECK(scsi_addr::decode(0x580200, false) == scsi_addr::kDackBit);
	CHECK(scsi_addr::decode(0x580260, false) == (scsi_addr::kDackBit | 6));
	CHECK(scsi_addr::decode(0x580060, false) == 6);
	CHECK(scsi_addr::decode(0x580200, true) == 0);
	CHECK(scsi_addr::decode(0x580260, true) == 6);
}

TEST_CASE("ncr5380 bus: select, IDENTIFY, TEST UNIT READY, status, message")
{
	Rig r;
	r.t.plans.push_back(ScriptedTarget::status());
	r.connect();
	r.h.sendCdb({0, 0, 0, 0, 0, 0});
	r.h.finish();
	REQUIRE(r.t.calls.size() == 1);
	CHECK(r.t.calls[0].lun == 0);
	CHECK(r.t.calls[0].cdb == std::vector<uint8_t>{0, 0, 0, 0, 0, 0});
}

TEST_CASE("ncr5380 bus: IDENTIFY carries the LUN")
{
	Rig r;
	r.t.plans.push_back(ScriptedTarget::status(scsi::Status::CheckCondition));
	REQUIRE(r.h.select(4, true));
	r.c.write(ODR, 0x83, false);
	r.c.write(ICR, I_DATA, false);
	r.c.write(ICR, I_DATA | I_ACK, false);
	r.c.write(ICR, 0, false);
	r.h.sendCdb({0x00, 0, 0, 0, 0, 0});
	CHECK(r.t.calls[0].lun == 3);
	CHECK(r.h.phase() == P_STATUS);
	CHECK(r.h.recvByte() == 0x02);
}

TEST_CASE("ncr5380 bus: selection without ATN skips message out")
{
	Rig r;
	r.t.plans.push_back(ScriptedTarget::status());
	REQUIRE(r.h.select(4, false));
	CHECK(r.h.phase() == P_CMD);
	r.h.sendCdb({0x00, 0x40, 0, 0, 0, 0});
	CHECK(r.t.calls[0].lun == 2); // from CDB byte 1 (SCSI-1 style)
}

TEST_CASE("ncr5380 bus: 10 byte CDB groups")
{
	Rig r;
	r.t.plans.push_back(ScriptedTarget::status());
	r.connect();
	r.h.sendCdb({0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0});
	REQUIRE(r.t.calls.size() == 1);
	CHECK(r.t.calls[0].cdb.size() == 10);
}

TEST_CASE("ncr5380 bus: data in by manual handshake")
{
	Rig r;
	r.t.plans.push_back(ScriptedTarget::dataIn({1, 2, 3, 4}));
	r.connect();
	r.h.sendCdb({0x12, 0, 0, 0, 4, 0});
	CHECK(r.h.phase() == P_DATAIN);
	for (uint8_t i = 1; i <= 4; i++) CHECK(r.h.recvByte() == i);
	r.h.finish();
}

TEST_CASE("ncr5380 bus: data in, two records")
{
	Rig r;
	r.t.plans.push_back(ScriptedTarget::dataIn({9, 8}));
	r.t.plans.push_back(ScriptedTarget::dataIn({7}));
	r.connect();
	r.h.sendCdb({0x08, 0, 0, 0, 0, 0});
	CHECK(r.h.recvByte() == 9);
	CHECK(r.h.recvByte() == 8);
	CHECK(r.h.phase() == P_DATAIN);
	CHECK(r.h.recvByte() == 7);
	r.h.finish();
}

TEST_CASE("ncr5380 bus: data out by manual handshake")
{
	Rig r;
	r.t.plans.push_back(ScriptedTarget::dataOut(3));
	r.t.plans.push_back(ScriptedTarget::status(scsi::Status::Good));
	r.connect();
	r.h.sendCdb({0x0A, 0, 0, 0, 3, 0});
	CHECK(r.h.phase() == P_DATAOUT);
	r.h.sendByte(0xAA);
	r.h.sendByte(0xBB);
	r.h.sendByte(0xCC);
	REQUIRE(r.t.received.size() == 1);
	CHECK(r.t.received[0] == std::vector<uint8_t>{0xAA, 0xBB, 0xCC});
	r.h.finish();
}

TEST_CASE("ncr5380 bus: pseudo-DMA data in")
{
	Rig r;
	r.t.plans.push_back(ScriptedTarget::dataIn({10, 20, 30, 40, 50}));
	r.connect();
	r.h.sendCdb({0x08, 0, 0, 0, 0, 0});
	r.c.write(TCR, P_DATAIN, false);
	r.c.write(MR, 0x02, false);
	CHECK((r.c.read(BSR, false) & 0x08) != 0); // phase match
	CHECK((r.c.read(BSR, false) & 0x40) == 0); // no DRQ before DMA start
	r.c.write(IDMARX, 0, false);
	CHECK((r.c.read(BSR, false) & 0x40) != 0);
	for (uint8_t v : {10, 20, 30, 40, 50}) CHECK(r.c.read(CDR, true) == v);
	CHECK((r.c.read(BSR, false) & 0x40) == 0); // DRQ gone: phase changed to status
	CHECK((r.c.read(BSR, false) & 0x08) == 0);
	r.c.write(MR, 0, false);
	r.c.write(TCR, P_STATUS, false);
	r.h.finish();
}

TEST_CASE("ncr5380 bus: pseudo-DMA data out")
{
	Rig r;
	r.t.plans.push_back(ScriptedTarget::dataOut(4));
	r.connect();
	r.h.sendCdb({0x0A, 0, 0, 0, 4, 0});
	r.c.write(TCR, P_DATAOUT, false);
	r.c.write(MR, 0x02, false);
	r.c.write(DMATX, 0, false);
	CHECK((r.c.read(BSR, false) & 0x40) != 0);
	for (uint8_t v : {1, 2, 3, 4}) r.c.write(ODR, v, true);
	REQUIRE(r.t.received.size() == 1);
	CHECK(r.t.received[0] == std::vector<uint8_t>{1, 2, 3, 4});
	r.c.write(MR, 0, false);
	r.c.write(TCR, P_STATUS, false);
	r.h.finish();
}

TEST_CASE("ncr5380 bus: selecting an empty ID never raises BSY")
{
	Rig r(4);
	CHECK_FALSE(r.h.select(2, true));
	for (int i = 0; i < 1000; i++) CHECK_FALSE(r.h.bsy());
	CHECK(r.t.calls.empty());
	// the real target is still selectable afterwards
	CHECK(r.h.select(4, true));
}

TEST_CASE("ncr5380 bus: ABORT message frees the bus")
{
	Rig r;
	REQUIRE(r.h.select(4, true));
	r.c.write(ODR, 0x06, false);
	r.c.write(ICR, I_DATA, false);
	r.c.write(ICR, I_DATA | I_ACK, false);
	r.c.write(ICR, 0, false);
	CHECK(r.t.aborts == 1);
	CHECK_FALSE(r.h.bsy());
}

TEST_CASE("ncr5380 bus: RST resets targets and frees the bus")
{
	int pokes = 0;
	ScriptedTarget t;
	Ncr5380::Lines l;
	l.onBusReset = [&] { pokes++; };
	Ncr5380 c(l);
	Host h{c};
	c.attach(4, &t);
	const int before = t.busResets;
	REQUIRE(h.select(4, false));
	c.write(ICR, 0x80, false);
	CHECK(t.busResets == before + 1);
	CHECK(pokes == 1);
	CHECK_FALSE(h.bsy());
	c.write(ICR, 0, false);
}

TEST_CASE("ncr5380 bus: detaching the last target restores the legacy stub")
{
	Ncr5380 c;
	ScriptedTarget t;
	c.attach(4, &t);
	CHECK(c.hasTargets());
	c.attach(4, nullptr);
	CHECK_FALSE(c.hasTargets());
	c.reset();
	c.write(ICR, 0x80, false);
	CHECK(c.read(BSR, false) == 0x10);
}
