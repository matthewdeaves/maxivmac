/*
	NCR5380 SCSI bus controller

	The legacy stub below is adapted from "SCSI.c" in vMac by Philip
	Cummins; NCR5380 chip emulation by Yoav Shadmi, 1998.
*/

#include "devices/ncr5380.h"

#define scsiRd 0x00
#define scsiWr 0x01

#define sCDR 0x00	 /* current scsi data register  (r/o) */
#define sODR 0x00	 /* output data register        (w/o) */
#define sICR 0x02	 /* initiator command register  (r/w) */
#define sMR 0x04	 /* mode register               (r/w) */
#define sTCR 0x06	 /* target command register     (r/w) */
#define sCSR 0x08	 /* current SCSI bus status     (r/o) */
#define sSER 0x08	 /* select enable register      (w/o) */
#define sBSR 0x0A	 /* bus and status register     (r/o) */
#define sDMAtx 0x0A	 /* start DMA send              (w/o) */
#define sIDR 0x0C	 /* input data register         (r/o) */
#define sTDMArx 0x0C /* start DMA target receive    (w/o) */
#define sRESET 0x0E	 /* reset parity/interrupt      (r/o) */
#define sIDMArx 0x0E /* start DMA initiator receive (w/o) */

Ncr5380::Ncr5380(Lines lines) : lines_(std::move(lines)) {}

void Ncr5380::reset()
{
	if (real_) realReset();
	for (int i = 0; i < kRegBytes; i++)
	{
		s_[i] = 0;
	}
}

/* Reset all NCR5380 registers to post-RST defaults and tell the host
   (via the onBusReset hook) that a reset occurred. */
void Ncr5380::legacyBusReset()
{
	s_[scsiRd + sCDR] = 0;
	s_[scsiWr + sODR] = 0;
	s_[scsiRd + sICR] = 0x80;
	s_[scsiWr + sICR] &= 0x80;
	s_[scsiRd + sMR] &= 0x40;
	s_[scsiWr + sMR] &= 0x40;
	s_[scsiRd + sTCR] = 0;
	s_[scsiWr + sTCR] = 0;
	s_[scsiRd + sCSR] = 0x80;
	s_[scsiWr + sSER] = 0;
	s_[scsiRd + sBSR] = 0x10;
	s_[scsiWr + sDMAtx] = 0;
	s_[scsiRd + sIDR] = 0;
	s_[scsiWr + sTDMArx] = 0;
	s_[scsiRd + sRESET] = 0;
	s_[scsiWr + sIDMArx] = 0;

	/* The missing piece of the puzzle.. :) */
	if (lines_.onBusReset) lines_.onBusReset();
}

/* Run the NCR5380 state machine: handle arbitration, selection,
   and bus-free phases based on written register values. */
void Ncr5380::legacyCheck()
{
	/*
		The arbitration select/reselect scenario
		[stub.. doesn't really work...]
	*/
	if ((s_[scsiWr + sODR] >> 7) == 1)
	{
		/* Check if the Mac tries to be an initiator */
		if ((s_[scsiWr + sMR] & 1) == 1)
		{
			/* the Mac set arbitration in progress */
			/*
				stub! tell the mac that there
				is arbitration in progress...
			*/
			s_[scsiRd + sICR] |= 0x40;
			/* ... that we didn't lose arbitration ... */
			s_[scsiRd + sICR] &= ~0x20;
			/*
				... and that there isn't a higher priority ID present...
			*/
			s_[scsiRd + sCDR] = 0x00;

			/*
				... the arbitration and selection/reselection is
				complete. the initiator tries to connect to the SCSI
				device, fails and returns after timeout.
			*/
		}
	}

	/* check the chip registers, AS SET BY THE CPU */
	if ((s_[scsiWr + sICR] >> 7) == 1)
	{
		/* Check Assert RST */
		legacyBusReset();
	}
	else
	{
		s_[scsiRd + sICR] &= ~0x80;
		s_[scsiRd + sCSR] &= ~0x80;
	}

	if ((s_[scsiWr + sICR] >> 2) == 1)
	{
		/* Check Assert SEL */
		s_[scsiRd + sCSR] |= 0x02;
		s_[scsiRd + sBSR] = 0x10;
	}
	else
	{
		s_[scsiRd + sCSR] &= ~0x02;
	}
}

uint8_t Ncr5380::read(uint8_t reg, bool dack)
{
	if (real_)
	{
		const uint8_t v = realRead(reg & 7, dack);
		syncLines();
		return v;
	}
	return s_[(reg & 7) * 2 + scsiRd];
}

void Ncr5380::write(uint8_t reg, uint8_t v, bool dack)
{
	if (real_)
	{
		realWrite(reg & 7, v, dack);
		syncLines();
		return;
	}
	s_[(reg & 7) * 2 + scsiWr] = v;
	legacyCheck();
}

/* ======================================================================
   Real bus model.  Register numbers (A0-A2):
	 read : 0 CSD  1 ICR  2 MR  3 TCR  4 CSBS  5 BSR  6 IDR  7 reset irq
	 write: 0 ODR  1 ICR  2 MR  3 TCR  4 SER   5 DMA tx 6 DMA tgt rx 7 DMA init rx
   ICR: 7 RST 6 AIP(r) 5 LA(r) 4 ACK 3 BSY 2 SEL 1 ATN 0 data bus enable
   MR : 6 target mode 2 monitor busy 1 DMA mode 0 arbitrate
   TCR: 2 MSG 1 C/D 0 I/O (expected phase)
   CSBS: 7 RST 6 BSY 5 REQ 4 MSG 3 C/D 2 I/O 1 SEL
   BSR: 7 end of DMA 6 DRQ 4 IRQ 3 phase match 2 busy error 1 ATN 0 ACK
   ====================================================================== */

namespace
{
constexpr uint8_t kIcrRst = 0x80, kIcrAip = 0x40, kIcrLa = 0x20, kIcrAck = 0x10, kIcrBsy = 0x08,
				  kIcrSel = 0x04, kIcrAtn = 0x02, kIcrData = 0x01;
constexpr uint8_t kMrDma = 0x02, kMrArb = 0x01, kMrMonBusy = 0x04;
constexpr uint8_t kBitIO = 1, kBitCD = 2, kBitMSG = 4;
} // namespace

void Ncr5380::attach(int id, scsi::ScsiTarget *t)
{
	if (id < 0 || id > 6) return;
	targets_[id] = t;
	bool any = false;
	for (auto *p : targets_) any = any || p != nullptr;
	if (any != real_)
	{
		real_ = any;
		realReset();
	}
}

void Ncr5380::realReset()
{
	odr_ = icr_ = mr_ = tcr_ = ser_ = 0;
	aip_ = la_ = false;
	phase_ = Phase::Free;
	req_ = ackPending_ = dmaActive_ = false;
	irq_ = endDma_ = busyErr_ = false;
	sel_ = -1;
	for (auto *t : targets_)
		if (t) t->busReset();
	syncLines();
}

void Ncr5380::realBusReset()
{
	for (auto *t : targets_)
		if (t) t->busReset();
	phase_ = Phase::Free;
	req_ = ackPending_ = dmaActive_ = false;
	sel_ = -1;
	aip_ = la_ = false;
	irq_ = true; /* RST on the bus interrupts the initiator */
	busyErr_ = false;
	if (lines_.onBusReset) lines_.onBusReset();
}

uint8_t Ncr5380::phaseBits() const
{
	switch (phase_)
	{
	case Phase::MsgOut: return kBitMSG | kBitCD;
	case Phase::Command: return kBitCD;
	case Phase::DataIn: return kBitIO;
	case Phase::Status: return kBitCD | kBitIO;
	case Phase::MsgIn: return kBitMSG | kBitCD | kBitIO;
	default: return 0; /* Free, Selected, DataOut */
	}
}

uint8_t Ncr5380::busData() const
{
	switch (phase_)
	{
	case Phase::DataIn: return inPos_ < in_.size() ? in_[inPos_] : 0;
	case Phase::Status: return statusByte_;
	case Phase::MsgIn: return 0; /* COMMAND COMPLETE */
	default: return (aip_ || (icr_ & kIcrData)) ? odr_ : 0;
	}
}

bool Ncr5380::dmaRequest() const
{
	return dmaActive_ && (mr_ & kMrDma) && req_ && phaseMatch();
}

void Ncr5380::setPhase(Phase p, bool req)
{
	const bool wasBusy = phase_ != Phase::Free;
	phase_ = p;
	req_ = req;
	if (p == Phase::Free)
	{
		/* BSY dropped.  With MONITOR BUSY set the chip interrupts and ends DMA. */
		if (wasBusy && (mr_ & kMrMonBusy))
		{
			irq_ = busyErr_ = true;
			dmaActive_ = false;
		}
		maybeArbitrate();
	}
}

/* Arbitration can only begin once the bus is free; the chip then drives
   BSY and its ID (ODR) onto the bus and reports AIP.  There is only one
   initiator on this bus, so nobody can beat us: LA is never set. */
void Ncr5380::maybeArbitrate()
{
	if (!(mr_ & kMrArb) || aip_) return;
	if (phase_ != Phase::Free || (icr_ & kIcrBsy)) return;
	aip_ = true;
	la_ = false;
}

void Ncr5380::checkSelect()
{
	if (phase_ != Phase::Free) return;
	if (!(icr_ & kIcrSel) || (icr_ & kIcrBsy) || aip_) return;
	const uint8_t data = (icr_ & kIcrData) ? odr_ : 0;
	for (int id = 0; id < 7; id++)
	{
		if ((data & (1 << id)) && targets_[id])
		{
			sel_ = id;
			identified_ = false;
			lun_ = 0;
			setPhase(Phase::Selected, false); /* target asserts BSY */
			return;
		}
	}
	/* Absent ID: nobody answers, BSY never rises, the driver times out. */
}

void Ncr5380::handleMessage(uint8_t m)
{
	if (m & 0x80)
	{
		identified_ = true;
		lun_ = m & 7;
	}
	else if (m == 0x06) /* ABORT */
	{
		targets_[sel_]->abort();
		setPhase(Phase::Free, false);
	}
	else if (m == 0x0C) /* BUS DEVICE RESET */
	{
		targets_[sel_]->busReset();
		setPhase(Phase::Free, false);
	}
	/* other messages (NOP etc.) are accepted and ignored */
}

void Ncr5380::enterStep(const scsi::Step &first)
{
	scsi::Step s = first;
	scsi::ScsiTarget *t = targets_[sel_];
	for (int guard = 0; guard < 4096; guard++)
	{
		switch (s.kind)
		{
		case scsi::Step::DataIn:
			if (s.in.empty())
			{
				s = t->dataInDone();
				continue;
			}
			in_ = s.in;
			inPos_ = 0;
			setPhase(Phase::DataIn, true);
			return;
		case scsi::Step::DataOut:
			if (s.out.empty())
			{
				s = t->dataOutDone();
				continue;
			}
			out_ = s.out;
			outPos_ = 0;
			setPhase(Phase::DataOut, true);
			return;
		case scsi::Step::StatusPhase:
			statusByte_ = static_cast<uint8_t>(s.status);
			setPhase(Phase::Status, true);
			return;
		}
	}
	/* Runaway target: terminate the command. */
	statusByte_ = static_cast<uint8_t>(scsi::Status::CheckCondition);
	setPhase(Phase::Status, true);
}

/* ACK asserted: the byte offered (or requested) in the current phase is
   transferred.  REQ drops. */
void Ncr5380::ackRise()
{
	if (!req_) return;
	switch (phase_)
	{
	case Phase::MsgOut: handleMessage(odr_); break;
	case Phase::Command:
		cdb_.push_back(odr_);
		if (cdb_.size() == 1)
		{
			const unsigned group = odr_ >> 5;
			cdbLen_ = group == 0 ? 6 : group == 1 || group == 2 ? 10 : group == 5 ? 12 : group == 4 ? 16 : 6;
		}
		break;
	case Phase::DataOut:
		if (outPos_ < out_.size()) out_[outPos_++] = odr_;
		break;
	case Phase::DataIn: inPos_++; break;
	default: break;
	}
	req_ = false;
	ackPending_ = true;
}

/* ACK released: the target moves on and asserts REQ for the next byte or
   phase. */
void Ncr5380::ackFall()
{
	if (!ackPending_) return;
	ackPending_ = false;
	scsi::ScsiTarget *t = sel_ >= 0 ? targets_[sel_] : nullptr;
	if (!t || phase_ == Phase::Free) return;
	switch (phase_)
	{
	case Phase::MsgOut:
		if (icr_ & kIcrAtn)
			req_ = true; /* more message bytes */
		else
		{
			cdb_.clear();
			setPhase(Phase::Command, true);
		}
		break;
	case Phase::Command:
		if (cdb_.size() < cdbLen_)
			req_ = true;
		else
		{
			const uint8_t lun = identified_ ? lun_ : (cdb_.size() > 1 ? cdb_[1] >> 5 : 0);
			enterStep(t->command(lun, cdb_));
		}
		break;
	case Phase::DataOut:
		if (outPos_ < out_.size())
			req_ = true;
		else
			enterStep(t->dataOutDone());
		break;
	case Phase::DataIn:
		if (inPos_ < in_.size())
			req_ = true;
		else
			enterStep(t->dataInDone());
		break;
	case Phase::Status: setPhase(Phase::MsgIn, true); break;
	case Phase::MsgIn:
		setPhase(Phase::Free, false);
		sel_ = -1;
		break;
	default: break;
	}
}

uint8_t Ncr5380::realRead(uint8_t reg, bool dack)
{
	switch (reg)
	{
	case 0:
	case 6:
		if (reg == 0 && dack && dmaRequest() && (phaseBits() & kBitIO))
		{
			const uint8_t b = busData();
			ackRise();
			ackFall();
			return b;
		}
		return busData();
	case 1: return (icr_ & ~(kIcrAip | kIcrLa)) | (aip_ ? kIcrAip : 0) | (la_ ? kIcrLa : 0);
	case 2: return mr_;
	case 3: return tcr_;
	case 4:
	{
		const bool bsy = phase_ != Phase::Free || (icr_ & kIcrBsy) || aip_;
		uint8_t v = icr_ & kIcrRst;
		if (bsy) v |= 0x40;
		if (req_) v |= 0x20;
		v |= static_cast<uint8_t>(phaseBits() << 2);
		if (icr_ & kIcrSel) v |= 0x02;
		return v;
	}
	case 5:
	{
		uint8_t v = 0;
		if (endDma_) v |= 0x80;
		if (dmaRequest()) v |= 0x40;
		if (irq_) v |= 0x10;
		if (phaseMatch()) v |= 0x08;
		if (busyErr_) v |= 0x04;
		if (icr_ & kIcrAtn) v |= 0x02;
		if (icr_ & kIcrAck) v |= 0x01;
		return v;
	}
	case 7: /* reading resets parity / interrupt / busy-error state */
		irq_ = busyErr_ = false;
		return 0;
	default: return 0;
	}
}

/* Evaluate DMA phase mismatch, then drive the IRQ/DRQ output lines. */
void Ncr5380::syncLines()
{
	/* A phase mismatch (REQ asserted for a phase other than TCR's) while a
	   DMA transfer is running ends the DMA and raises an interrupt. */
	if (dmaActive_ && (mr_ & kMrDma) && req_ && !phaseMatch())
	{
		dmaActive_ = false;
		endDma_ = true;
		irq_ = true;
	}
	const bool drq = dmaRequest();
	if (irq_ != irqOut_)
	{
		irqOut_ = irq_;
		if (lines_.irq) lines_.irq(irqOut_);
	}
	if (drq != drqOut_)
	{
		drqOut_ = drq;
		if (lines_.drq) lines_.drq(drqOut_);
	}
}

void Ncr5380::realWrite(uint8_t reg, uint8_t v, bool dack)
{
	switch (reg)
	{
	case 0:
		odr_ = v;
		if (dack && dmaRequest() && !(phaseBits() & kBitIO))
		{
			ackRise();
			ackFall();
		}
		break;
	case 1:
	{
		const uint8_t old = icr_;
		icr_ = v & ~(kIcrAip | kIcrLa);
		if ((v & kIcrRst) && !(old & kIcrRst))
		{
			realBusReset();
			break;
		}
		if (!(old & kIcrAck) && (v & kIcrAck)) ackRise();
		if ((old & kIcrAck) && !(v & kIcrAck)) ackFall();
		if (phase_ == Phase::Selected && !(v & kIcrSel))
		{
			cdb_.clear();
			setPhase((v & kIcrAtn) ? Phase::MsgOut : Phase::Command, true);
		}
		checkSelect();
		break;
	}
	case 2:
	{
		const uint8_t old = mr_;
		mr_ = v;
		if ((v & kMrArb) && !(old & kMrArb)) maybeArbitrate();
		if (!(v & kMrArb)) aip_ = la_ = false;
		if (!(v & kMrDma)) dmaActive_ = endDma_ = false;
		checkSelect();
		break;
	}
	case 3: tcr_ = v & 0x0F; break;
	case 4: ser_ = v; break;
	case 5:
	case 6:
	case 7:
		if (mr_ & kMrDma)
		{
			dmaActive_ = true;
			endDma_ = false;
		}
		break;
	}
}
