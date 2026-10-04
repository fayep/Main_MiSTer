# MiSTer_pdp2011: host-side compatibility for images found in the wild

Status (2026-10-04): 1 (streamed tapes) and 3 (TU58) done and hardware-tested on feature/tape-compat; 2 planned.

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

**Done (2026-10-04, b1e4e14): streamed, not converted.**
- **Detection** on mount, slot 3, from the first records only: SIMH (lengths
  match their trailers), then E11 (SIMH framing, an odd record unpadded),
  then TPC (16-bit chain). E11 is tried before TPC because E11 data can pass
  as a TPC chain. A full walk is only the fallback for odd files, e.g. a
  SIMH tape with junk after its last record (the RSTS/E 10.1 kit), which is
  served as SIMH.
- **TPC/E11:** the original file is mounted read-only and the slot's sector
  reads are assembled on the fly (`pdp2011_tape_read`).
  - An index of records (virtual SIMH offset, source offset, length) is
    built from headers only, as far as reads reach.
  - Each block is header + data + pad + trailer; past the last record comes
    an EOM marker (FFFFFFFF), which the TM11 reports as EOT.
  - The core uses img_size only as "mounted", so the source size is
    reported.
- **History:** a first version converted into games/PDP2011/.converted at
  mount. Walking 21 MB before mounting let the PDP-11 boot before the tape
  was there.
- **Host test:** `tapeconv --stream src dst`.

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

## 3. TU58 DECtape II on the serial line (OSD: "Line 1 = TU58")

**Goal (user).** One of the serial options attaches a TU58 emulator to the
serial line automatically.

**Wiring (built 2026-10-04).**
- **Core** (PDP2011_MiSTer `feature/serial-lineclock`): OSD "Line 1 (176500):
  Serial port / TU58 DECtape II" (status bit 17). It is hidden when Console =
  Serial, because only with the VT console does line 1 (176500, vector 300,
  the standard TU58 address) reach the UART. The core routes nothing
  differently: the bit is for Main.
- **Main** (`support/pdp2011/tu58.cpp`): every 0.5 s, if Console = VT and
  Line 1 = TU58, run `tu58fs` on /dev/ttyS1; otherwise, or when another core
  loads, stop it.
  - Files are in `games/PDP2011/tu58/`: the `tu58fs` program, and the
    cartridges `dd0.dsk` and `dd1.dsk`, created as empty RT-11 tapes if
    missing. The log is /tmp/tu58fs.log.
  - Main re-execs itself on every core load, so it keeps /tmp/tu58fs.pid and
    adopts or stops a leftover emulator.
  - It refuses to start, with an OSD message, if Main's own UART mode is not
    None, since that also uses ttyS1.
- **tu58fs** (`~/Source/tu58fs`, branch `mister-sigterm`): SIGTERM/SIGINT now
  exit like `Q`, saving changed cartridges. Before this, a kill lost writes
  made within the 3 s sync timeout.

**Speeds.** KL11 lines 0 and 1 run at 19200 baud (`kl0_bps` and `kl1_bps` in
mister_top; the divisor is 186 x 14 samples at 50 MHz = 19201 baud). The
core's config string declares `UART19200`, so Main sets ttyS1 to 19200.
`tu58fs -b 19200` matches both.

**Testing.** pdp_harness holds ttyS1 and must not run while the TU58 is
attached: two readers split the bytes. That is the likely cause of the 2
October XXDP `DIR DD0:` hang. The console is then the VT, so tests type
through a virtual uinput keyboard (`claude_vtype.py`) and read the screen
with screenshots.

## Order

1. **TPC conversion:** smallest, already done by hand three times, removes a
   whole class of support questions.
2. **Bad-block tables:** needs the drive-geometry rules above, then a test
   with a blank `truncate`d RL02 and standalone BRU (it must restore without
   the hand-written table).
3. **TU58:** the biggest (emulator choice, UART routing, OSD), and it changes
   what the console can be.
