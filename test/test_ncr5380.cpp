// Tests for the NCR5380 model (src/devices/ncr5380.*).
#include <doctest/doctest.h>

#include "devices/ncr5380.h"
#include "devices/scsi_addr.h"
#include "scsi_host.h"
#include "scsi_scripted_target.h"

#include <memory>

namespace
{
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

TEST_CASE("ncr5380 bus: selection works at the lowest and highest IDs")
{
	for (int id : {0, 1, 5, 6})
	{
		Rig r(id);
		r.t.plans.push_back(ScriptedTarget::status());
		r.connect(id);
		r.h.sendCdb({0, 0, 0, 0, 0, 0});
		r.h.finish();
		CHECK(r.t.calls.size() == 1);
	}
}

TEST_CASE("ncr5380 bus: several targets, selection picks the addressed one")
{
	ScriptedTarget a, b;
	Ncr5380 c;
	Host h{c};
	c.attach(0, &a);
	c.attach(6, &b);
	b.plans.push_back(ScriptedTarget::status());
	REQUIRE(h.select(6, false));
	h.sendCdb({0, 0, 0, 0, 0, 0});
	CHECK(a.calls.empty());
	CHECK(b.calls.size() == 1);
}

TEST_CASE("ncr5380 arbitration: AIP set when bus free, LA clear, our ID on the bus")
{
	Rig r;
	r.c.write(ODR, 0x80, false);
	r.c.write(MR, 0x01, false);
	CHECK((r.c.read(ICR, false) & 0x60) == 0x40);
	CHECK(r.c.read(CDR, false) == 0x80);
	CHECK(r.h.bsy()); // the chip drives BSY while arbitrating
	r.c.write(MR, 0x00, false);
	CHECK((r.c.read(ICR, false) & 0x60) == 0x00);
	CHECK_FALSE(r.h.bsy());
}

TEST_CASE("ncr5380 arbitration: waits for bus free")
{
	Rig r;
	r.t.plans.push_back(ScriptedTarget::status());
	r.connect();
	r.c.write(MR, 0x01, false); // bus is busy with the target
	CHECK((r.c.read(ICR, false) & 0x40) == 0);
	r.h.sendCdb({0, 0, 0, 0, 0, 0});
	r.c.write(MR, 0x01, false);
	CHECK((r.c.read(ICR, false) & 0x40) == 0);
	CHECK(r.h.recvByte() == 0); // status
	CHECK((r.c.read(ICR, false) & 0x40) == 0);
	CHECK(r.h.recvByte() == 0); // COMMAND COMPLETE: bus goes free
	CHECK((r.c.read(ICR, false) & 0x40) != 0); // arbitration now proceeds
}

TEST_CASE("ncr5380 arbitration: RST clears AIP")
{
	Rig r;
	r.c.write(ODR, 0x80, false);
	r.c.write(MR, 0x01, false);
	r.c.write(ICR, 0x80, false);
	CHECK((r.c.read(ICR, false) & 0x40) == 0);
}

TEST_CASE("ncr5380 irq: phase mismatch at end of DMA data-in ends DMA and raises IRQ")
{
	std::vector<bool> irqs, drqs;
	ScriptedTarget t;
	Ncr5380::Lines l;
	l.irq = [&](bool v) { irqs.push_back(v); };
	l.drq = [&](bool v) { drqs.push_back(v); };
	Ncr5380 c(l);
	Host h{c};
	c.attach(4, &t);
	t.plans.push_back(ScriptedTarget::dataIn({1, 2, 3}));
	REQUIRE(h.select(4, false));
	h.sendCdb({0x08, 0, 0, 0, 0, 0});
	c.write(TCR, P_DATAIN, false);
	c.write(MR, 0x02, false);
	CHECK(drqs.empty());
	c.write(IDMARX, 0, false);
	REQUIRE(drqs.size() == 1);
	CHECK(drqs.back());
	CHECK((c.read(BSR, false) & 0x10) == 0);
	CHECK(irqs.empty());
	CHECK(c.read(CDR, true) == 1);
	CHECK(c.read(CDR, true) == 2);
	CHECK(irqs.empty());
	CHECK(c.read(CDR, true) == 3);
	// target moved to STATUS: phase mismatch
	CHECK(irqs == std::vector<bool>{true});
	CHECK(drqs == std::vector<bool>{true, false});
	const uint8_t bsr = c.read(BSR, false);
	CHECK((bsr & 0x80) != 0); // end of DMA
	CHECK((bsr & 0x10) != 0); // IRQ
	CHECK((bsr & 0x40) == 0); // DRQ
	CHECK((bsr & 0x08) == 0); // phase mismatch
	// further DMA reads no longer transfer
	CHECK(c.read(CDR, true) == 0x00); // status byte, not acknowledged
	CHECK(h.phase() == P_STATUS);
	// reading the reset register clears the interrupt
	c.read(RESETR, false);
	CHECK((c.read(BSR, false) & 0x10) == 0);
	CHECK(irqs == std::vector<bool>{true, false});
	c.write(MR, 0, false);
	c.write(TCR, P_STATUS, false);
	h.finish();
}

TEST_CASE("ncr5380 irq: phase mismatch after DMA data-out")
{
	Rig r;
	r.t.plans.push_back(ScriptedTarget::dataOut(2));
	r.connect();
	r.h.sendCdb({0x0A, 0, 0, 0, 2, 0});
	r.c.write(TCR, P_DATAOUT, false);
	r.c.write(MR, 0x02, false);
	r.c.write(DMATX, 0, false);
	r.c.write(ODR, 7, true);
	CHECK((r.c.read(BSR, false) & 0x10) == 0);
	r.c.write(ODR, 8, true);
	CHECK((r.c.read(BSR, false) & 0x90) == 0x90);
	r.c.write(MR, 0, false);
	CHECK((r.c.read(BSR, false) & 0x80) == 0); // end-of-DMA clears with DMA mode
}

TEST_CASE("ncr5380 irq: monitor-busy interrupt when the target releases BSY")
{
	Rig r;
	r.t.plans.push_back(ScriptedTarget::status());
	r.connect();
	r.h.sendCdb({0, 0, 0, 0, 0, 0});
	r.c.write(MR, 0x04, false); // MONITOR BUSY
	CHECK((r.c.read(BSR, false) & 0x14) == 0);
	CHECK(r.h.recvByte() == 0);
	CHECK((r.c.read(BSR, false) & 0x14) == 0);
	CHECK(r.h.recvByte() == 0); // COMMAND COMPLETE, bus free
	CHECK((r.c.read(BSR, false) & 0x14) == 0x14);
	r.c.read(RESETR, false);
	CHECK((r.c.read(BSR, false) & 0x14) == 0);
}

TEST_CASE("ncr5380 irq: bus RST raises IRQ")
{
	std::vector<bool> irqs;
	ScriptedTarget t;
	Ncr5380::Lines l;
	l.irq = [&](bool v) { irqs.push_back(v); };
	Ncr5380 c(l);
	c.attach(4, &t);
	c.write(ICR, 0x80, false);
	CHECK(irqs == std::vector<bool>{true});
	c.write(ICR, 0, false);
	c.read(RESETR, false);
	CHECK(irqs == std::vector<bool>{true, false});
}

TEST_CASE("ncr5380 lines stay silent in legacy mode")
{
	int n = 0;
	Ncr5380::Lines l;
	l.irq = [&](bool) { n++; };
	l.drq = [&](bool) { n++; };
	Ncr5380 c(l);
	c.write(ICR, 0x80, false);
	c.write(MR, 0x02, false);
	c.write(IDMARX, 0, false);
	CHECK(n == 0);
}
