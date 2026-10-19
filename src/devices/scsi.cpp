/*
	Small Computer System Interface EMulated DeVice

	Emulates the SCSI found in the Mac Plus.  This is the memory-mapped
	adapter; the chip itself lives in ncr5380.{h,cpp}.
*/

#include "core/common.h"

#include "devices/scsi.h"

static Ncr5380::Lines makeLines()
{
	Ncr5380::Lines l;
	/* The missing piece of the puzzle.. :) -- tell the Mac's low-memory
	   hardware flags that SCSI reset occurred. */
	l.onBusReset = []() { put_ram_word(0xb22, get_ram_word(0xb22) | 0x8000); };
	return l;
}

SCSIDevice::SCSIDevice() : bus_(makeLines()) {}

void SCSIDevice::reset()
{
	bus_.reset();
}

uint32_t SCSIDevice::access(uint32_t Data, bool WriteMem, uint32_t addr)
{
	if (addr < 8)
	{
		if (WriteMem)
		{
			bus_.write(static_cast<uint8_t>(addr), static_cast<uint8_t>(Data), false);
		}
		else
		{
			Data = bus_.read(static_cast<uint8_t>(addr), false);
		}
	}
	return Data;
}
