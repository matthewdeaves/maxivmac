# Real Mac SE: DaynaPORT + MacTCP checklist

Target: a real Mac SE, System 6.0.8, MacTCP 2.0.x, with a SCSI adapter that
emulates the Dayna SCSI/Link (BlueSCSI, or similar). What the emulator has
shown (`NETWORKING_TESTING.md`, "The same flow on the Mac SE"): the stock
Dayna driver, EtherTalk, MacTCP 2.0.6 and MacTCP Ping 2.0.2 work on `--model=MacSE`
against `--scsi4=daynaport --eth=slirp`: 5 of 5 echoes to the gateway, twice.

**Marking:** statements about real BlueSCSI/SCSI2SD firmware or hardware are
from memory and not checked here; they are tagged *(unverified)*. Statements
about the emulator are from the traces and tests of this repository.

## 1. On the Mac SE

1. **System 6.0.8** with the **Dayna SCSI/Link driver and AppleTalk 58.0.0**
   from Dayna's installer (it adds `Network`, `AppleTalk`, `EtherTalk Phase 2`
   and `Responder` to the System Folder). Use *Custom Install*: "AppleTalk
   v58.0.0 for System 6" and "DaynaPORT SCSI/Link".
2. **Chooser:** AppleTalk **Active**.
3. **Control Panel > Network:** choose **EtherTalk Alternative** (it appears
   once the adapter answers on the SCSI bus). This must be done **before**
   MacTCP is in the System Folder: with MacTCP present at boot the switch fails
   ("Could not switch to EtherTalk", `Open ".ATP"` = -108 memFullErr, FINDINGS
   F24). Then copy `MacTCP` into the System Folder and **restart**. The system
   heap is the limit, not the RAM size, so a bigger SE does not help (the
   emulator gave the same heap at 1, 2 and 4 MB).
4. **Control Panel > MacTCP:** pick **Ethernet Built-In** (not LocalTalk),
   *More...*: manual address, netmask, gateway, and a DNS server with a domain
   (MacTCP refuses a DNS entry with a blank domain). Close the panel (this
   writes `MacTCP Prep`), **restart** again.
5. Test with MacTCP Ping. TCP and name resolution were **not** exercised in the
   emulator (no TCP client on the disk; DNS configured but no name resolved).
6. Do not rely on InterSLIP being installed at the same time: it costs system
   heap that EtherTalk needs (F24).

The emulator's slirp network is 10.0.2.0/24 (guest 10.0.2.15, gateway
10.0.2.2, DNS 10.0.2.3); on the real LAN use real values.

## 2. On the adapter

- **SCSI ID:** any free ID other than the SE's own (7) and the boot disk's. The
  emulator tests used 4. The Dayna driver finds the adapter at boot with a
  1-byte `READ(6)` (no INQUIRY at boot, FINDINGS F7); which IDs it scans is not
  known here, so if the adapter is not found, try a different ID *(unverified)*.
- **Device type:** the adapter has to be configured as a DaynaPORT / network
  device, not as a disk. BlueSCSI v2 firmware has this (network on the Pico W
  boards, set in its `.ini`) *(unverified: names and options of the settings)*.
  A plain SCSI2SD image cannot be a network adapter as far as I know
  *(unverified)*; check the firmware's feature list before buying.
- **Bus:** termination and term-power are the usual SE SCSI rules (the SE
  internal bus is terminated; the last external device terminates) *(unverified
  for this adapter)*. Cable and power problems look like a vanished adapter, not
  like network errors.
- **Driver identity:** the real adapter must identify as `Dayna   SCSI/Link
  2.0f` (INQUIRY, 37 bytes), which is what the Dayna driver was seen to receive
  from the emulator. Whether the real driver checks the revision is unknown.

## 3. DaynaPORT behaviour: emulator vs. real adapters

What the Dayna driver sent in the emulator (SE and Plus are identical): bus
reset, `READ(6)` 1 byte probe (CHECK CONDITION), INQUIRY, ENABLE (0x0E), GET MAC
(0x09), SET MULTICAST (0x0D), WRITE (0x0A, framed with a 4-byte header and a
trailing zero record), `READ(6)` 1524 bytes with flag 0xC0 polled continuously.
It never sent REQUEST SENSE, MODE SENSE or vendor command 0x80.

| Topic | Emulator | Real BlueSCSI / others |
|---|---|---|
| Frames per READ | one by default; `--scsiN=daynaport,multiframe` packs several | BlueSCSI: one frame per READ *(unverified)* |
| "More data" flag (last flag byte) | 0x10 on a record when more frames are queued (FINDINGS F18) | PiSCSI may use `0x00000001` *(unverified)*; the real driver was only seen with one frame pending |
| READ when empty | 6-byte zero header | same, as far as I know *(unverified)* |
| Receive timing | a frame is shown to the guest 10 ms (emulated) after it is queued (`--eth-latency`); 0 or 5 ms makes MacTCP Ping lose replies | a real LAN or Wi-Fi bridge answers in at least a few ms; if replies come back faster than that, expect the same Ping behaviour *(unverified)* |
| FCS | 4 bytes appended to each frame in READ | same layout in the Dayna protocol *(unverified on hardware)* |
| Oversized frame | dropped, READ returns the empty header | unknown |
| Link down | not an error (TUR ok, READ empty) | unknown |

Differences to expect only on real hardware, none of which the emulator can
show: SCSI bus timing and phase handling of the adapter's microcontroller
(the emulator asserts REQ with zero latency), the SE's real pseudo-DMA/blind
transfer handling, Wi-Fi or bridge latency and loss, and adapter firmware that
does not implement a command the driver uses.

## 4. If it does not work, in this order

1. Does the Network cdev offer EtherTalk (adapter found)? No: SCSI ID, cable,
   termination, power, device type.
2. EtherTalk switch fails with an alert: system heap (step 1.3, MacTCP not yet
   in the System Folder, no other INITs).
3. MacTCP shows no "Ethernet Built-In": the Network cdev is still on LocalTalk,
   or the Dayna driver is not in the System Folder.
4. Ping to the gateway: first echo arrives, later ones time out: replies too
   fast (see receive timing), or the adapter drops frames.
5. Compare with the emulator's trace: `--scsi4=daynaport --diag=scsi` shows the
   exact command sequence the driver should produce.

## 5. Emulator-side notes for the SE

- Run SE network tests at `--speed=0`: AppleTalk start-up crashes above 1x
  (FINDINGS F41); a real SE is not affected.
- A Restart on the SE needs the F42 fix.
- The Mac II family is not supported for SCSI devices (F19); the SE is.
