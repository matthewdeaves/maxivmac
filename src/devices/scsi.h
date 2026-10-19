/*
	SCSI — SCSI bus controller (MMIO adapter around the NCR5380 model)
*/
#pragma once

#include "devices/device.h"
#include "devices/ncr5380.h"
#include <cstdint>

class SCSIDevice : public Device
{
public:
	SCSIDevice();

	uint32_t access(uint32_t data, bool writeMem, uint32_t addr) override;
	void zap() override {}
	void reset() override;
	const char *name() const override { return "SCSI"; }

private:
	Ncr5380 bus_;
};
