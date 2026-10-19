/*
	ScsiTarget: the seam between the NCR5380 bus model and a SCSI device.

	The bus (Ncr5380) owns registers, phases, arbitration/selection, the
	REQ/ACK handshake, pseudo-DMA and all messages.  A target only sees
	whole CDBs and whole data records, and answers with the next "step".
	Everything is synchronous and in emulated time.
*/
#pragma once

#include <cstddef>
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
	std::span<const uint8_t> in;  // DataIn: target-owned, valid until the next call into the target
	std::span<uint8_t> out;		  // DataOut: target-owned, filled by the bus
	Status status = Status::Good;
};

class ScsiTarget
{
public:
	virtual ~ScsiTarget() = default;

	// CDB length for `opcode`, or 0 to use the standard length for its group.
	// Lets a target with vendor opcodes (e.g. 0x40, 0x80) say how long they are.
	virtual size_t cdbLength(uint8_t /*opcode*/) const { return 0; }
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
