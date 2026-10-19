/*
	DaynaPORT SCSI/Link Ethernet adapter, as a scsi::ScsiTarget.

	The command set (INQUIRY, REQUEST SENSE, READ 0x08, GET MAC 0x09,
	WRITE 0x0A, receive mode 0x0C, multicast 0x0D, ENABLE 0x0E, ...) and the
	record layouts follow the Mini vMac DaynaPORT fork by Mikhail Kandel
	(src/DAYNAPRT.h), GPL-2.0-only, which in turn follows BlueSCSI-v2's
	network.c and the PiSCSI "Dayna Port Command Set" notes.  This is a
	re-implementation as a target on maxivmac's own NCR5380 bus model; the
	donor's NCR5380 (SCSIDNPT.h) is not used.

	SPDX-License-Identifier: GPL-2.0-only
*/
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "devices/eth_backend.h"
#include "devices/scsi_target.h"

class DaynaPortTarget : public scsi::ScsiTarget
{
public:
	using Mac = std::array<uint8_t, 6>;
	// Stable, locally administered (bit 1 of the first octet set).
	static constexpr Mac kDefaultMac = {0x02, 0x6D, 0x78, 0x76, 0x6D, 0x01};

	explicit DaynaPortTarget(std::unique_ptr<EthernetBackend> eth, Mac mac = kDefaultMac);

	size_t cdbLength(uint8_t opcode) const override { return (opcode == 0x40 || opcode == 0x80) ? 6 : 0; }
	scsi::Step command(uint8_t lun, std::span<const uint8_t> cdb) override;
	scsi::Step dataOutDone() override;
	void busReset() override;
	void abort() override;
	void tick() override;
	const char *name() const override { return "daynaport"; }

	// Adapter statistics; saturating, so they never wrap.
	struct Stats
	{
		uint32_t rxFrames = 0;	  // frames delivered to the guest
		uint32_t txFrames = 0;	  // frames sent to the backend
		uint32_t txDropped = 0;	  // guest frames dropped (link down or backend refused)
		uint32_t rxFiltered = 0;  // received frames not addressed to this adapter
	};
	const Stats &stats() const { return stats_; }
	// Frames lost to receive-queue overflow in the backend.
	uint32_t rxDropped() const;

	EthernetBackend &backend() { return *eth_; }
	const Mac &mac() const { return mac_; }
	const std::vector<Mac> &multicast() const { return multicast_; }

private:
	// Transfer buffer: bounds a packed READ and the largest DATA OUT.
	static constexpr size_t kBufSize = 16384;

	static constexpr size_t kMaxMulticast = 64;

	bool accepts(std::span<const uint8_t> frame) const;
	scsi::Step handle(uint8_t lun);
	scsi::Step error(uint8_t senseKey);
	scsi::Step dataIn(size_t n);
	scsi::Step dataOut(size_t n);
	scsi::Step done();

	std::unique_ptr<EthernetBackend> eth_;
	Mac mac_;
	std::vector<Mac> multicast_;
	bool allMulticast_ = false;
	std::array<uint8_t, 16> cdb_{};
	std::vector<uint8_t> buf_ = std::vector<uint8_t>(kBufSize);
	size_t length_ = 0;
	uint8_t sense_ = 0;
	Stats stats_;
	bool enabled_ = false;
	bool writeHeader_ = false;
};
