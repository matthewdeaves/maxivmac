// Shared helpers for tests that drive an Ncr5380 the way a SCSI Manager
// driver does: register/bit names and a Host that arbitrates, selects and
// does manual REQ/ACK transfers.
#pragma once

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>

#include "devices/ncr5380.h"

namespace scsit
{
// Register numbers (A0-A2).
enum : uint8_t
{
	CDR = 0, ODR = 0, ICR = 1, MR = 2, TCR = 3, CSR = 4, SER = 4, BSR = 5, IDR = 6,
	RESETR = 7, DMATX = 5, TDMARX = 6, IDMARX = 7
};

// ICR bits
inline constexpr uint8_t I_ACK = 0x10, I_BSY = 0x08, I_SEL = 0x04, I_ATN = 0x02, I_DATA = 0x01;
// CSBS bits
inline constexpr uint8_t C_BSY = 0x40, C_REQ = 0x20;
// phases as MSG<<2|CD<<1|IO
inline constexpr uint8_t P_DATAOUT = 0, P_DATAIN = 1, P_CMD = 2, P_STATUS = 3, P_MSGOUT = 6, P_MSGIN = 7;

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

} // namespace scsit

using namespace scsit;
