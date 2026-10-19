/*
	NCR5380 SCSI bus controller

	Self-contained (no dependency on the rig or on emulated memory) so
	that it can be linked into the unit-test binary.

	Register numbers are the chip's A0-A2 select lines (0..7).  Reads and
	writes of the same number address different registers, exactly as on
	the real chip.  `dack` is the pseudo-DMA /DACK line (Mac address bit
	A9); it is ignored by the legacy stub.

	The legacy register-array stub from the original Mini vMac code
	(Yoav Shadmi 1998 / Philip Cummins) is preserved bit-for-bit.
*/
#pragma once

#include <cstdint>
#include <functional>
#include <utility>

class Ncr5380
{
public:
	struct Lines
	{
		std::function<void(bool)> irq;	   // IRQ output level changed
		std::function<void(bool)> drq;	   // DRQ output level changed
		std::function<void()> onBusReset;  // RST asserted via ICR (host kludge hook)
	};

	explicit Ncr5380(Lines lines = {});

	uint8_t read(uint8_t reg, bool dack);
	void write(uint8_t reg, uint8_t v, bool dack);
	void reset();

private:
	// ---- legacy stub ----
	static constexpr int kRegBytes = 0x10;
	uint8_t s_[kRegBytes] = {};
	void legacyBusReset();
	void legacyCheck();

	Lines lines_;
};
