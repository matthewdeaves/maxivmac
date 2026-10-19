/*
	SCSI MMIO address decode (pure function, unit-testable).

	On the compact Macs the NCR5380 register select lines are A4-A6 and
	the pseudo-DMA /DACK line is A9.  The result is packed as the
	device-local address handed to SCSIDevice::access(): bits 0-2 are the
	register number, bit 3 is /DACK.

	The Mac II family uses a different pseudo-DMA scheme (separate
	handshake regions); /DACK is not decoded there.
*/
#pragma once

#include <cstdint>

namespace scsi_addr
{
constexpr uint32_t kDackBit = 0x8;

constexpr uint32_t decode(uint32_t addr, bool iiFamily)
{
	uint32_t a = (addr >> 4) & 0x07;
	if (!iiFamily && (addr & 0x200) != 0) a |= kDackBit;
	return a;
}
} // namespace scsi_addr
