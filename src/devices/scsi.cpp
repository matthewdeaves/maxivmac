/*
	Small Computer System Interface EMulated DeVice

	Emulates the SCSI found in the Mac Plus.  This is the memory-mapped
	adapter; the chip itself lives in ncr5380.{h,cpp}.
*/

#include "core/common.h"

#include "devices/scsi.h"
#include "devices/scsi_addr.h"
#include "devices/via2.h"
#include "core/rig.h"

static Ncr5380::Lines makeLines(SCSIDevice *self)
{
	Ncr5380::Lines l;
	/* Mac II family: the 5380's IRQ is wired to VIA2 CA2 and DRQ to VIA2 CB2
	   (both negative-edge inputs).  Compact Macs have no such wiring.  Lines
	   only fire while a target is attached, so this is inert otherwise. */
	l.irq = [self](bool level) { if (level) self->pulseVia2(true); };
	l.drq = [self](bool level) { if (level) self->pulseVia2(false); };
	/* The missing piece of the puzzle.. :) -- tell the Mac's low-memory
	   hardware flags that SCSI reset occurred. */
	l.onBusReset = []() { put_ram_word(0xb22, get_ram_word(0xb22) | 0x8000); };
	return l;
}

SCSIDevice::SCSIDevice() : bus_(makeLines(this)) {}

void SCSIDevice::pulseVia2(bool irq)
{
	if (!rig_ || !rig_->config().isIIFamily()) return;
	if (auto *via2 = rig_->findDevice<VIA2Device>())
	{
		if (irq) via2->iCA2_PulseNtfy();
		else via2->iCB2_PulseNtfy();
	}
}

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

void SCSIDevice::tick()
{
	for (auto &t : targets_)
		if (t) t->tick();
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
