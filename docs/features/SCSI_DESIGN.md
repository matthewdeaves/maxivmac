# SCSI design

## Layers

```
Mac CPU  --MMIO-->  SCSIDevice (src/devices/scsi.cpp)       address adapter, owns targets
                      |  scsi_addr::decode: reg = A4-A6, dack = A9
                      v
                    Ncr5380 (src/devices/ncr5380.cpp)       registers, bus, phases, DMA, IRQ/DRQ
                      |  scsi::ScsiTarget (scsi_target.h)
                      v
                    target (disk, DaynaPORT, test ScriptedTarget)
```

`Ncr5380` has no dependency on the rig, memory or any global, so it is
linked straight into the unit-test binary. Host effects are injected via
`Ncr5380::Lines`:

| Callback | Meaning | Host wiring |
|----------|---------|-------------|
| `irq(bool)` | IRQ output level changed | Mac II family: VIA2 CA2 pulse on rising edge |
| `drq(bool)` | DRQ output level changed | Mac II family: VIA2 CB2 pulse on rising edge |
| `onBusReset()` | RST asserted via ICR | sets bit 15 of low-memory 0xB22 (SCSI present), as the stub always did |

## Legacy stub vs real bus

`Ncr5380::attach()` switches the chip to the real model when the first
target is attached and back to the stub when the last is removed. With no
target the register file is the original 16-byte array and every read and
write behaves bit-for-bit as before. This matters because the state
recorder CRCs every I/O access and the boot ROM probes the chip; any change
to the no-target behaviour would change all goldens.

## ScsiTarget

```cpp
Step command(uint8_t lun, std::span<const uint8_t> cdb);
Step dataOutDone();      // one DataOut record received; may return another step
Step dataInDone();       // one DataIn record consumed; default: STATUS
void busReset(); void abort(); const char *name() const;
```

`cdbLength(opcode)` lets a target with vendor opcodes (DaynaPORT 0x40/0x80)
override the standard group-based CDB length (0 = standard). `tick()` is
called once per emulated 1/60 s tick (`SCSIDevice::tick`) so a target can pump
host I/O.

A `Step` is `DataIn` (bus sends the target-owned `in` span), `DataOut` (bus
fills the target-owned `out` span, then calls `dataOutDone`) or
`StatusPhase` (status byte, then COMMAND COMPLETE). The bus owns phases,
selection, messages and handshakes; a target never sees them. Everything is
synchronous in emulated time: REQ is asserted with zero latency and nothing
is paced by the wall clock.

## Bus model

Phase machine: `Free -> Selected -> [MsgOut] -> Command -> (DataIn | DataOut)* -> Status -> MsgIn -> Free`.

- **Arbitration.** MR.ARBITRATE with the bus free sets AIP, drives BSY and
  the ODR value (our ID) onto the data bus. If the bus is busy, AIP is set
  when it frees. LA is never set (one initiator).
- **Selection.** SEL asserted with BSY released and a data-bus bit matching
  an attached target selects it (BSY rises, visible in CSBS). The target
  proceeds when the initiator drops SEL: to MESSAGE OUT if ATN is asserted,
  else straight to COMMAND (LUN then comes from CDB byte 1).
- **Handshake.** ACK rising consumes the byte offered or requested (data
  captured from ODR for output phases); ACK falling advances to the next
  byte or phase and re-asserts REQ. MESSAGE OUT repeats while ATN is
  asserted at ACK release. CDB length comes from the opcode group.
- **Pseudo-DMA.** After MR.DMA and a write to the start-DMA register, a
  register-0 access with /DACK performs a whole REQ/ACK cycle. DRQ is
  `DMA active && REQ && phase match`.
- **Phase mismatch.** If REQ is asserted for a phase other than TCR's while
  DMA runs, DMA ends, BSR end-of-DMA and IRQ are set. This is how a driver
  learns that a DATA transfer finished (the target moved to STATUS).
- **Other IRQs.** MONITOR BUSY with BSY dropping, and bus RST. Reading
  register 7 clears IRQ and the busy-error bit.

## Behavioural reference

The Mini vMac DaynaPORT fork (`SCSIDNPT.h`, GPL-2.0-only) implements one
target at ID 4 with shared globals; its register and phase sequencing was
used as a reference for what the System 6/7 Dayna driver needs. The bus
here is a generalised model with a target interface and shares no code with
it. The DaynaPORT *protocol* logic in `scsi_daynaport.cpp` is derived from
the donor's `DAYNAPRT.h` (Mikhail Kandel, GPL-2.0-only) and carries its
attribution.
The legacy stub retains its original attribution (Yoav Shadmi 1998, Philip
Cummins).

## Open questions

- Real-chip behaviour of the MR DMA bit on a phase mismatch (cleared by the
  chip or left to the driver) is not verified; we leave it set.
- End-of-DMA (BSR bit 7) is set on phase mismatch and cleared by turning
  DMA mode off or starting a new DMA; the datasheet ties it to EOP.
- Mac II family pseudo-DMA addressing is not decoded.
