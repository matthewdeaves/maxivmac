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
#include <span>
#include <utility>
#include <vector>

#include "devices/scsi_target.h"

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

	// Attach a target at SCSI ID 0..6 (non-owning; nullptr detaches).  ID 7
	// is the initiator and cannot be attached.  While no target is attached
	// the chip runs the legacy stub exactly as before.
	void attach(int id, scsi::ScsiTarget *t);
	bool hasTargets() const { return real_; }

	uint8_t read(uint8_t reg, bool dack);
	void write(uint8_t reg, uint8_t v, bool dack);
	void reset();

private:
	// ---- legacy stub ----
	static constexpr int kRegBytes = 0x10;
	uint8_t s_[kRegBytes] = {};
	void legacyBusReset();
	void legacyCheck();

	// ---- real bus (used only while a target is attached) ----
	enum class Phase : uint8_t { Free, Selected, MsgOut, Command, DataIn, DataOut, Status, MsgIn };

	uint8_t realRead(uint8_t reg, bool dack);
	void realWrite(uint8_t reg, uint8_t v, bool dack);
	void realReset();
	void realBusReset();
	uint8_t phaseBits() const;	// MSG<<2 | C/D<<1 | I/O of the current bus phase
	uint8_t busData() const;
	bool phaseMatch() const { return phaseBits() == (tcr_ & 7); }
	bool dmaRequest() const;
	void ackRise();
	void ackFall();
	void enterStep(const scsi::Step &s);
	void setPhase(Phase p, bool req);
	void checkSelect();
	void handleMessage(uint8_t m);
	void maybeArbitrate();

	scsi::ScsiTarget *targets_[7] = {};
	bool real_ = false;
	uint8_t odr_ = 0, icr_ = 0, mr_ = 0, tcr_ = 0, ser_ = 0;
	bool aip_ = false, la_ = false;
	Phase phase_ = Phase::Free;
	bool req_ = false, ackPending_ = false;
	bool dmaActive_ = false;
	int sel_ = -1;
	uint8_t lun_ = 0;
	bool atnResume_ = false;		 // MESSAGE OUT was entered because ATN was asserted
	Phase resumePhase_ = Phase::Free; // phase to return to afterwards
	bool identified_ = false;
	std::vector<uint8_t> cdb_;
	size_t cdbLen_ = 0;
	std::span<const uint8_t> in_;
	size_t inPos_ = 0;
	std::span<uint8_t> out_;
	size_t outPos_ = 0;
	uint8_t statusByte_ = 0;

	Lines lines_;
};
