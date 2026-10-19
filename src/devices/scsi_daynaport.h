/*
	DaynaPORT SCSI/Link Ethernet adapter, as a scsi::ScsiTarget.

	The command set (INQUIRY, REQUEST SENSE, READ 0x08, GET MAC 0x09,
	WRITE 0x0A, receive mode 0x0C, multicast 0x0D, ENABLE 0x0E, ...) and the
	record layouts follow the Mini vMac DaynaPORT fork by Mikhail Kandel
	(src/DAYNAPRT.h), GPL-2.0-only.  The protocol facts (command opcodes, the
	record layout, flag values) are as documented by BlueSCSI-v2 and the
	PiSCSI "Dayna Port Command Set" notes, which the donor cites as its
	references; no BlueSCSI or PiSCSI source is copied here.  This is a
	re-implementation as a target on maxivmac's own NCR5380 bus model; the
	donor's NCR5380 (SCSIDNPT.h) is not used.

	Provenance: https://github.com/MikhailKandel/minivmac, branch
	feature/daynaport-vmnet, commits dd3db14 and ed3364c (DAYNAPRT.h and
	tests/daynaport.c).  No upstream issue or pull request is known;
	if the fork was offered to Mini vMac upstream, add the reference here.
	maxivmac is GPL-2.0-only (see LICENSE); so is this file.

	SPDX-License-Identifier: GPL-2.0-only
*/
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "devices/eth_backend.h"
#include "devices/scsi_target.h"

struct DaynaPortOptions
{
	// Pack several queued frames into one READ.  Off by default: one frame per
	// READ, as BlueSCSI-v2 does.  Packing follows the Mini vMac fork and is
	// unverified against the real driver (see the flag note in the .cpp).
	bool multiFrame = false;
};

class DaynaPortTarget : public scsi::ScsiTarget
{
public:
	using Options = DaynaPortOptions;
	using Mac = std::array<uint8_t, 6>;
	// Stable, locally administered (bit 1 of the first octet set).
	static constexpr Mac kDefaultMac = {0x02, 0x6D, 0x78, 0x76, 0x6D, 0x01};

	explicit DaynaPortTarget(std::unique_ptr<EthernetBackend> eth, Mac mac = kDefaultMac, Options opt = {});

	size_t cdbLength(uint8_t opcode) const override { return (opcode == 0x40 || opcode == 0x80) ? 6 : 0; }
	scsi::Step command(uint8_t lun, std::span<const uint8_t> cdb) override;
	scsi::Step dataOutDone() override;
	scsi::Step dataInDone() override;
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
		uint32_t rxOversize = 0;  // received frames dropped: too big for the guest's READ allocation
		uint32_t rxLost = 0;	  // frames already dequeued for a READ that bus reset/ABORT cut short
	};
	const Stats &stats() const { return stats_; }
	// Frames lost to receive-queue overflow in the backend.
	uint32_t rxDropped() const;

	EthernetBackend &backend() { return *eth_; }
	const Mac &mac() const { return mac_; }
	const Options &options() const { return opt_; }
	const std::vector<Mac> &multicast() const { return multicast_; }

private:
	// Transfer buffer: bounds a packed READ and the largest DATA OUT.
	static constexpr size_t kBufSize = 16384;

	static constexpr size_t kMaxMulticast = 64;

	bool accepts(std::span<const uint8_t> frame) const;
	scsi::Step handle(uint8_t lun);
	scsi::Step handleDataOut();
	scsi::Step error(uint8_t senseKey);
	scsi::Step dataIn(size_t n);
	scsi::Step dataOut(size_t n);
	scsi::Step done();

	std::unique_ptr<EthernetBackend> eth_;
	Mac mac_;
	Mac defaultMac_;
	Options opt_;
	std::vector<Mac> multicast_;
	bool allMulticast_ = false;
	std::array<uint8_t, 16> cdb_{};
	std::vector<uint8_t> buf_ = std::vector<uint8_t>(kBufSize);
	size_t length_ = 0;
	uint8_t sense_ = 0;
	Stats stats_;
	bool enabled_ = false;
	bool writeHeader_ = false;
	uint32_t readFrames_ = 0; // frames in the READ data phase not yet fully transferred
	void loseInFlight();
};
