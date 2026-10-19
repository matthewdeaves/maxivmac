/*
	DaynaPORT SCSI/Link Ethernet adapter, as a scsi::ScsiTarget.

	The command set (INQUIRY, REQUEST SENSE, READ 0x08, GET MAC 0x09,
	WRITE 0x0A, receive mode 0x0C, multicast 0x0D, ENABLE 0x0E, ...) and the
	record layouts follow the Mini vMac DaynaPORT fork by Mikhail Kandel
	(src/DAYNAPRT.h), GPL-2.0-only, which in turn follows BlueSCSI-v2's
	network.c and the PiSCSI "Dayna Port Command Set" notes.  This is a
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

	EthernetBackend &backend() { return *eth_; }
	const Mac &mac() const { return mac_; }

private:
	static constexpr size_t kBufSize = kEthMaxFrame + 10;

	scsi::Step handle(uint8_t lun);
	scsi::Step error(uint8_t senseKey);
	scsi::Step dataIn(size_t n);
	scsi::Step dataOut(size_t n);
	scsi::Step done();

	std::unique_ptr<EthernetBackend> eth_;
	Mac mac_;
	std::array<uint8_t, kBufSize> buf_{};
	std::array<uint8_t, 16> cdb_{};
	size_t length_ = 0;
	uint8_t sense_ = 0;
	bool enabled_ = false;
	bool writeHeader_ = false;
};
