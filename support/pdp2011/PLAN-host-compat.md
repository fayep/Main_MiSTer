# MiSTer_pdp2011: host-side compatibility for images found in the wild

Status (2026-10-04): 1 implemented and hardware-tested (feature/tape-compat); 2 and 3 planned.

People will mount whatever images they find on the internet. Main (the
PDP2011 edition) should make the common variants just work, so the core
stays a faithful PDP-11 and never has to guess. All three items below are
gated on `is_pdp2011()`.

## 1. Tapes: TPC (and E11) -> SIMH .tap on mount

**Problem.** The TM11 reads only the SIMH format (32-bit little-endian record
length, data padded to even, the length again; 0 = tape mark; FFFFFFFF = end
of medium). Many kits are TPC (16-bit length, data, no trailing length):
`rsts_v9_6_install.tap`, `cobol-81_v2_3.tap` and `cobol-11_v4_4.tap` all had
to be converted by hand.

**Plan.** In `user_io_file_mount()`, for the tape slot (index 3):

- **Detect** the format by walking the whole file:
  - **SIMH:** every leading length equals its trailing copy, ending exactly
    at EOF or at FFFFFFFF;
  - **TPC:** a chain of 16-bit lengths with data padded to even, ending
    exactly at EOF;
  - **E11:** like SIMH but odd records unpadded.
  
  If the file is ambiguous, prefer SIMH.
- **If not SIMH:** convert to
  `games/PDP2011/.converted/<name>.<size>-<mtime>.tap` (reused while the
  source is unchanged) and mount that read-only. Show "TPC tape converted" on
  the OSD info line.
- **Never touch the original** file.

Converter: the same logic as the Python used by hand (records and marks
round-trip identical), in C++, streaming (tapes can be large).

## 2. Disks: an empty DEC STD 144 bad-block table where the image has none

**Problem.** RL01/RL02 (and RK06/07, RM02/03/05/80) packs carry a factory
bad-sector table on the last track: serial number, two zero words, entries,
`177777` terminators. SIMH writes one with "no bad blocks" when it creates an
image (`sim_disk_pdp11_bad_block`). Images made any other way (`truncate`,
other emulators, some archives) have zeros there. BRU then fails ("Manufacturer
bad sector file is corrupt") and RSX/RT-11 init can misbehave. Some images are
also short: the RSTS RP06 images here are 6 sectors short of 815*19*22*512.

**Plan.** In the sector-read path (`UIO_SECTOR_RD` for the disk indexes):

- **RL slot:** an image of exactly 5242880 (RL01) or 10485760 (RL02) bytes, or
  those plus SIMH's 512-byte footer. When the core reads a block of the last
  track (the last 20 blocks of 512 bytes = 40 RL sectors) and that block is
  **all zero**, return the synthesized table instead:
  - serial = CRC of the file name, as SIMH;
  - 0, 0;
  - 177777 fill.
  
  The block is never written to the file. Once the OS writes the last track,
  the real data is returned as normal.
- **RH slot** for RM-type drives: the same rule, keyed by geometry, once the
  core reports the drive type. RP04/05/06 had no DEC 144 table, so nothing is
  synthesized for them.
- **Short images (decided with the user, 2026-10-04):** reads past EOF
  return zeros, as now. A write past EOF on a disk slot **grows the file**
  (zero-filled) up to the drive's nominal size, like SIMH.
  - **Exception:** a write that lands only in the last-track bad-block-table
    region never grows the file. The synthesized "no bad blocks" table stays
    virtual, and the image keeps its size.
  - Growth never exceeds the drive's nominal size; beyond that the write is
    dropped, as today.

Rule of thumb: **only ever synthesize where the image has nothing.** Any
non-zero data on the last track is the OS's and is returned unchanged.

## 3. TU58 DECtape II on the serial line (OSD: "Serial: TU58")

**Goal (user).** One of the serial options attaches a TU58 emulator to the
serial line automatically.

**Wiring.** The core has one UART (Linux `/dev/ttyS1`).
- With the console on the virtual VT100, KL11 line 1 (176500, vector 300)
  goes to the UART. That is the standard TU58 address: RT-11 `DD:`, RSX
  `DD:`, XXDP `DD`.
- A new UART mode, "TU58", in the `uartmode` scheme (`/sbin/uartmode N`,
  `GetUARTbaud`) starts a TU58 emulator on `/dev/ttyS1` at the line speed
  (TU58: 38400 typical; RSP is speed-agnostic).
- **Emulator:** tu58fs (Jörg Hoppe), or a small C emulator built into Main.
  Either way it must speak RSP and MRSP (boot ROMs use MRSP).
- **Cartridges:** `games/PDP2011/tu58/` (default `dd0.dsk`, `dd1.dsk`, 256 KB
  each, created on first use), selectable from the OSD later.
- **Constraint:** the serial console and TU58 share the one UART, so TU58
  needs the VT console. The OSD should say so, or force VT when TU58 is
  picked.
- **Testing:** our pdp_harness also uses the UART, so tests must drive the
  VT console (keyboard) or a second channel. Plan that before building.

## Order

1. **TPC conversion:** smallest, already done by hand three times, removes a
   whole class of support questions.
2. **Bad-block tables:** needs the drive-geometry rules above, then a test
   with a blank `truncate`d RL02 and standalone BRU (it must restore without
   the hand-written table).
3. **TU58:** the biggest (emulator choice, UART routing, OSD), and it changes
   what the console can be.
