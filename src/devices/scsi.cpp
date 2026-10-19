/*
	Small Computer System Interface EMulated DeVice

	Emulates the SCSI found in the Mac Plus.  This is the memory-mapped
	adapter; the chip itself lives in ncr5380.{h,cpp}.
*/

#include "core/common.h"

#include "devices/scsi.h"
#include "devices/scsi_addr.h"

static Ncr5380::Lines makeLines()
{
	Ncr5380::Lines l;
	/* The missing piece of the puzzle.. :) -- tell the Mac's low-memory
	   hardware flags that SCSI reset occurred. */
	l.onBusReset = []() { put_ram_word(0xb22, get_ram_word(0xb22) | 0x8000); };
	return l;
}

SCSIDevice::SCSIDevice() : bus_(makeLines()) {}

void SCSIDevice::attachTarget(int id, std::unique_ptr<scsi::ScsiTarget> target)
{
	if (id < 0 || id > 6) return;
	bus_.attach(id, target.get());
	targets_[id] = std::move(target);
}

scsi::ScsiTarget *SCSIDevice::target(int id) const
{
	return (id < 0 || id > 6) ? nullptr : targets_[id].get();
}

void SCSIDevice::reset()
{
	bus_.reset();
}

uint32_t SCSIDevice::access(uint32_t Data, bool WriteMem, uint32_t addr)
{
	/* addr: bits 0-2 register number, bit 3 pseudo-DMA /DACK (A9). */
	const uint8_t reg = addr & 7;
	const bool dack = (addr & scsi_addr::kDackBit) != 0;
	if (WriteMem)
	{
		bus_.write(reg, static_cast<uint8_t>(Data), dack);
	}
	else
	{
		Data = bus_.read(reg, dack);
	}
	return Data;
}
