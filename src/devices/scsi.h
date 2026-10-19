/*
	SCSI — SCSI bus controller (MMIO adapter around the NCR5380 model)
*/
#pragma once

#include "devices/device.h"
#include "devices/ncr5380.h"
#include "devices/scsi_target.h"
#include <array>
#include <cstdint>
#include <memory>

class SCSIDevice : public Device
{
public:
	SCSIDevice();

	uint32_t access(uint32_t data, bool writeMem, uint32_t addr) override;
	void zap() override {}
	void reset() override;
	const char *name() const override { return "SCSI"; }

	// Attach a target at SCSI ID 0..6 (takes ownership; nullptr removes).
	// While no target is attached the device behaves as the legacy stub.
	void attachTarget(int id, std::unique_ptr<scsi::ScsiTarget> target);
	scsi::ScsiTarget *target(int id) const;

	// Pulse VIA2 CA2 (IRQ) or CB2 (DRQ); Mac II family only.
	void pulseVia2(bool irq);

private:
	Ncr5380 bus_;
	std::array<std::unique_ptr<scsi::ScsiTarget>, 7> targets_;
};
