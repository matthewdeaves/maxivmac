// Tests for the NCR5380 model (src/devices/ncr5380.*).
#include <doctest/doctest.h>

#include "devices/ncr5380.h"

namespace
{
// Register numbers (A0-A2).
enum : uint8_t
{
	CDR = 0, ODR = 0, ICR = 1, MR = 2, TCR = 3, CSR = 4, SER = 4, BSR = 5, IDR = 6,
	RESETR = 7, DMATX = 5, TDMARX = 6, IDMARX = 7
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
