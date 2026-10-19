/*
	ScsiTarget: the seam between the NCR5380 bus model and a SCSI device.

	The bus (Ncr5380) owns registers, phases, arbitration/selection, the
	REQ/ACK handshake, pseudo-DMA and all messages.  A target only sees
	whole CDBs and whole data records, and answers with the next "step".
	Everything is synchronous and in emulated time.
*/
#pragma once

#include <cstdint>
#include <span>

namespace scsi
{
enum class Status : uint8_t
{
	Good = 0x00,
	CheckCondition = 0x02,
	Busy = 0x08
};

struct Step
{
	enum Kind : uint8_t
	{
		DataIn,		 // bus sends `in` to the initiator
		DataOut,	 // bus fills `out` from the initiator, then calls dataOutDone()
		StatusPhase	 // bus sends `status` then COMMAND COMPLETE
	} kind = StatusPhase;
	// DataIn: target-owned.  Valid until the target's next command(),
	// dataInDone(), dataOutDone(), busReset() or abort(); tick() may run while
	// the bus is still transferring it and must leave the data alone.
	std::span<const uint8_t> in;
	std::span<uint8_t> out;		  // DataOut: target-owned, filled by the bus (same lifetime)
	Status status = Status::Good;
};

class ScsiTarget
{
public:
	virtual ~ScsiTarget() = default;

	// A complete CDB arrived.
	virtual Step command(uint8_t lun, std::span<const uint8_t> cdb) = 0;
	// One DataOut record has been received; may return another DataOut.
	virtual Step dataOutDone() = 0;
	// One DataIn record has been consumed; may return another DataIn.
	virtual Step dataInDone() { return Step{Step::StatusPhase, {}, {}, Status::Good}; }

	virtual void busReset() = 0;
	virtual void abort() {}
	virtual const char *name() const = 0;
};
} // namespace scsi
