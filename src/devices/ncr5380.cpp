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

uint8_t Ncr5380::read(uint8_t reg, bool /*dack*/)
{
	return s_[(reg & 7) * 2 + scsiRd];
}

void Ncr5380::write(uint8_t reg, uint8_t v, bool /*dack*/)
{
	s_[(reg & 7) * 2 + scsiWr] = v;
	legacyCheck();
}
