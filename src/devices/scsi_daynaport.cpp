/*
	DaynaPORT SCSI/Link target.  See scsi_daynaport.h for provenance.

	Protocol logic derived from Mini vMac DaynaPORT fork by Mikhail Kandel,
	src/DAYNAPRT.h (GPL-2.0-only).
	SPDX-License-Identifier: GPL-2.0-only
*/

#include "devices/scsi_daynaport.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/diag.h"
#define SCSI_LOG(fmt, ...) DIAG(SCSI, fmt "\n", ##__VA_ARGS__)

namespace
{
uint32_t crc32(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xFFFFFFFFu;
	while (n--)
	{
		crc ^= *p++;
		for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
	}
	return ~crc;
}

enum : uint8_t
{
	kTestUnitReady = 0x00,
	kRequestSense = 0x03,
	kRead = 0x08,
	kWrite = 0x0A,
	kInquiry = 0x12,
	kModeSense = 0x1A,
	kGetMac = 0x09,
	kSetMode = 0x0C,
	kSetMulticast = 0x0D,
	kEnable = 0x0E,
	kVendor80 = 0x80
};
constexpr size_t kReadHeader = 6; // size:2 + flags:4
// Low byte of the flags word in a READ record header: more frames follow.
constexpr uint8_t kMoreFlag = 0x10;
constexpr uint8_t kIllegalRequest = 5;
constexpr uint8_t kNotReady = 2;
constexpr uint8_t kHardwareError = 4;

void bump(uint32_t &c)
{
	if (c != UINT32_MAX) c++;
}

std::string hex(std::span<const uint8_t> b)
{
	std::string s;
	char t[4];
	for (uint8_t x : b)
	{
		snprintf(t, sizeof t, "%02X ", x);
		s += t;
	}
	if (!s.empty()) s.pop_back();
	return s;
}
} // namespace

uint32_t DaynaPortTarget::rxDropped() const
{
	return uint32_t(std::min<uint64_t>(eth_->droppedFrames(), UINT32_MAX));
}

DaynaPortTarget::DaynaPortTarget(std::unique_ptr<EthernetBackend> eth, Mac mac)
	: eth_(std::move(eth)), mac_(mac)
{
}

scsi::Step DaynaPortTarget::dataIn(size_t n)
{
	length_ = n;
	if (!n) return done();
	scsi::Step s;
	s.kind = scsi::Step::DataIn;
	s.in = std::span<const uint8_t>(buf_.data(), n);
	return s;
}

scsi::Step DaynaPortTarget::dataOut(size_t n)
{
	length_ = n;
	if (!n) return done();
	scsi::Step s;
	s.kind = scsi::Step::DataOut;
	s.out = std::span<uint8_t>(buf_.data(), n);
	return s;
}

scsi::Step DaynaPortTarget::done()
{
	length_ = 0;
	return scsi::Step{scsi::Step::StatusPhase, {}, {}, scsi::Status::Good};
}

scsi::Step DaynaPortTarget::error(uint8_t senseKey)
{
	sense_ = senseKey;
	length_ = 0;
	writeHeader_ = false;
	return scsi::Step{scsi::Step::StatusPhase, {}, {}, scsi::Status::CheckCondition};
}

scsi::Step DaynaPortTarget::command(uint8_t lun, std::span<const uint8_t> cdb)
{
	cdb_.fill(0);
	std::copy_n(cdb.begin(), std::min(cdb.size(), cdb_.size()), cdb_.begin());
	scsi::Step s = handle(lun);
	SCSI_LOG("dayna: lun %u CDB %s -> %s len %zu status %u", lun, hex(cdb).c_str(),
			 s.kind == scsi::Step::DataIn ? "DATA-IN" : s.kind == scsi::Step::DataOut ? "DATA-OUT" : "STATUS",
			 s.kind == scsi::Step::DataIn ? s.in.size() : s.kind == scsi::Step::DataOut ? s.out.size() : 0,
			 unsigned(s.status));
	return s;
}

scsi::Step DaynaPortTarget::handle(uint8_t lun)
{
	const size_t size = (size_t(cdb_[3]) << 8) | cdb_[4];
	writeHeader_ = false;
	std::fill(buf_.begin(), buf_.end(), 0);
	if (lun != 0 && cdb_[0] != kInquiry) return error(kIllegalRequest);

	switch (cdb_[0])
	{
		case kTestUnitReady:
			// Present means ready; link state is not a SCSI condition.
			return done();
		case kRequestSense:
			// Dayna's zero-allocation REQUEST SENSE returns nine bytes.
			buf_[0] = 0x70;
			buf_[2] = sense_;
			buf_[7] = 1;
			sense_ = 0;
			return dataIn(cdb_[4] ? std::min<size_t>(cdb_[4], 9) : 9);
		case kInquiry:
			if (cdb_[1] & 1) return error(kIllegalRequest);
			buf_[0] = lun ? 0x7F : 3;
			buf_[2] = 1;
			buf_[4] = 31;
			std::memcpy(buf_.data() + 8, "Dayna   SCSI/Link       2.0f", 28);
			return dataIn(std::min<size_t>(cdb_[4], 37));
		case kRead:
		{
			// The boot-time size-one probe must fail without a data phase.
			if (size < 6) return error(kIllegalRequest);
			// Pack as many whole records as fit, each [size:2][flags:4][frame+FCS].
			// A frame that does not fit stays queued.  The "more" flag is set in
			// a record's header when another frame follows it (in this transfer
			// or still queued), so the driver knows to keep reading.
			const size_t room = std::min(size, buf_.size());
			size_t total = 0, last = 0;
			while (enabled_ && eth_->linkUp())
			{
				const size_t pk = eth_->peekSize();
				if (!pk) break;
				// Ethernet minimum excludes FCS; backends may omit padding.
				const size_t n = std::max<size_t>(pk, 60);
				const size_t rec = kReadHeader + n + 4;
				if (total + rec > room)
				{
					if (!total) return error(kIllegalRequest); // frame stays queued
					break;
				}
				uint8_t *r = buf_.data() + total;
				eth_->receive(std::span<uint8_t>(r + kReadHeader, pk));
				const uint32_t crc = crc32(r + kReadHeader, n);
				for (int i = 0; i < 4; i++) r[kReadHeader + n + i] = uint8_t(crc >> (8 * i));
				bump(stats_.rxFrames);
				r[0] = uint8_t((n + 4) >> 8);
				r[1] = uint8_t(n + 4);
				if (total) buf_[last + 5] |= kMoreFlag;
				last = total;
				total += rec;
			}
			if (total && eth_->peekSize()) buf_[last + 5] |= kMoreFlag;
			return dataIn(total ? total : kReadHeader);
		}
		case kGetMac:
			// MAC, then three big-endian counters: frame alignment errors, CRC
			// errors (neither can occur here) and frames lost.
			std::copy(mac_.begin(), mac_.end(), buf_.begin());
			{
				const uint32_t lost = rxDropped();
				for (int i = 0; i < 4; i++) buf_[14 + i] = uint8_t(lost >> (24 - 8 * i));
			}
			// Hardware returns all 18 bytes even for the diagnostics size-six CDB.
			return dataIn(18);
		case kWrite:
			if (!enabled_) return error(kNotReady);
			if (cdb_[5] == 0)
			{
				if (size < kEthHeaderLen || size > kEthMaxFrame) return error(kIllegalRequest);
				return dataOut(size);
			}
			if (cdb_[5] == 0x80)
			{
				// Four-byte record headers; a zero-length record terminates.
				writeHeader_ = true;
				return dataOut(4);
			}
			return error(kIllegalRequest);
		case kSetMode:
			if (cdb_[5] == 0x40) return error(kIllegalRequest); // MAC override: unsupported
			return done();
		case kSetMulticast:
			if (size > kBufSize || size % 6) return error(kIllegalRequest);
			return dataOut(size);
		case kEnable:
			enabled_ = (cdb_[5] & 0x80) != 0;
			// Discard old packets on either transition, with bounded work.
			for (int i = 0; i < 64 && eth_->peekSize(); i++)
				eth_->receive(std::span<uint8_t>(buf_.data(), kEthMaxFrame));
			return done();
		case kModeSense: // matches BlueSCSI's no-data MODE SENSE
		case kVendor80:
			return done();
		default:
			return error(kIllegalRequest);
	}
}

// Called after each complete DATA OUT record, not just at command end.
scsi::Step DaynaPortTarget::dataOutDone()
{
	if (cdb_[0] == kWrite)
	{
		if (writeHeader_)
		{
			const size_t n = (size_t(buf_[0]) << 8) | buf_[1];
			if (!n) return done();
			if (n < kEthHeaderLen || n > kEthMaxFrame) return error(kIllegalRequest);
			writeHeader_ = false;
			return dataOut(n);
		}
		// Cable unplugged: the frame goes nowhere, as on real hardware.
		if (!eth_->linkUp())
			bump(stats_.txDropped);
		else if (!eth_->send(std::span<const uint8_t>(buf_.data(), length_)))
		{
			bump(stats_.txDropped);
			return error(kHardwareError);
		}
		else
			bump(stats_.txFrames);
		if (cdb_[5] == 0x80)
		{
			writeHeader_ = true;
			return dataOut(4);
		}
	}
	return done();
}

void DaynaPortTarget::busReset()
{
	enabled_ = false;
	sense_ = 0;
	length_ = 0;
	writeHeader_ = false;
}

void DaynaPortTarget::abort()
{
	length_ = 0;
	writeHeader_ = false;
}

void DaynaPortTarget::tick()
{
	eth_->poll();
}
