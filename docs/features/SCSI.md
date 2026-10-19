# SCSI

maxivmac emulates the NCR5380 SCSI controller found in the Mac Plus, SE,
Classic and (with a different pseudo-DMA scheme) the Mac II family.

## Current behaviour

By default no SCSI device is attached. In that case the controller runs
the original Mini vMac stub: it answers the boot ROM's probing and bus
reset, and selecting any target ID simply times out. This path is
deliberately unchanged so that boot behaviour, and the golden-file I/O
CRCs in `test/*.golden`, do not move.

As soon as a target is attached to the bus (`SCSIDevice::attachTarget`)
the stub is replaced by a real NCR5380 bus model:

- arbitration (AIP) and selection at any target ID 0-6; the Mac is ID 7
- IDENTIFY and the other messages (ABORT, BUS DEVICE RESET, COMMAND COMPLETE)
- COMMAND, DATA IN, DATA OUT, STATUS and MESSAGE IN phases
- the manual REQ/ACK handshake and pseudo-DMA (blind transfers using /DACK,
  Mac address bit A9)
- phase-mismatch interrupt, MONITOR BUSY, IRQ and DRQ output lines
- selecting an ID with no target never raises BSY, so the driver's
  selection timeout fires (deterministically, at any emulation speed)

One target type ships: the DaynaPORT SCSI/Link Ethernet adapter
(`--scsiN=daynaport`, see [NETWORKING_DAYNAPORT.md](NETWORKING_DAYNAPORT.md)).
`--scsiN=none` (N = 0-6) leaves the ID empty. The seam is designed so that
a SCSI disk can be added as a self-contained class; see
[SCSI_DESIGN.md](SCSI_DESIGN.md). `--diag=scsi` traces every CDB a target sees.

## Known limits

- Only one initiator (the Mac) exists, so the LA (lost arbitration) bit is
  never set and reselection is not modelled; targets cannot disconnect.
- Parity, EOP and target mode are not implemented.
- On the Mac II family the IRQ and DRQ lines are delivered to VIA2 CA2 and
  CB2, but no real driver has exercised that path, and /DACK pseudo-DMA
  (A9) is decoded for the compact Macs only.
- Verified by unit tests (`test/test_ncr5380.cpp`) driving register
  sequences, and end to end by the real Dayna driver on System 6.0.8
  talking to the DaynaPORT target (selection, IDENTIFY, CDBs, data in and
  out, status, bus resets); see NETWORKING_DAYNAPORT.md for the trace.

## Testing

`test/test_ncr5380.cpp` drives the chip with the same register sequences a
SCSI Manager driver uses, against `test/scsi_scripted_target.h`, a target
that records the CDBs it receives and plays canned data and status.
It links only `src/devices/ncr5380.cpp`, so it needs no emulator rig.
