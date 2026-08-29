#pragma once
#include <stdint.h>

/*
 * DDR3 shared-memory layout for XU native networking (PDP2011 DEUNA/DELUA
 * emulation, ENC424J600 SPI shim). See /Users/faye/.claude/plans/
 * keen-sauteeing-dolphin.md for the full design history.
 *
 * Unlike A2065 (LANCE protocol runs on the ARM side, needs a register
 * ring), DEUNA's protocol runs entirely on-chip in xu.vhd's own embedded
 * CPU -- this window is a direct 1:1 mirror of the real ENC424J600's own
 * buffer+register model (Microchip DS39935C S9.0), not an invented ring.
 * Same physical base as A2065/ne2000 (0x1FF00000, the first byte of the
 * region u-boot's mem=511M reserves from Linux) -- no collision, since
 * MiSTer runs one core's bitstream at a time.
 *
 * Byte offset -> Avalon word address (rtl/xu_ddr_mailbox.vhd's own
 * convention, matches A2065's): Avalon = (ARM_byte_offset >> 3).
 */

#define XU_DDR3_BASE          0x1FF00000UL
#define XU_DDR3_WINDOW_SIZE   0x10000UL

/* ── Packet buffer (shared FPGA shim + ARM daemon, 1:1 mirror of the real
 * chip's own 24KB SRAM buffer -- exact split from roms/xubrt45.mac's own
 * erxst init value, #44000 octal = 0x4800) ────────────────────────────── */
#define XU_BUF_TX_OFF          0x0000UL   /* TX / general-purpose, 18KB */
#define XU_BUF_TX_SIZE         0x4800UL
#define XU_BUF_RX_OFF          0x4800UL   /* RX ring, 6KB */
#define XU_BUF_RX_SIZE         0x1800UL

/* ── Control fields, each in its own 8-byte-aligned slot (one writer
 * each, per the ownership analysis in the plan -- never pack two fields
 * with different writers into the same word) ───────────────────────── */
#define XU_RXEN_OFF            0x6000UL   /* shim writes once at init, daemon reads */
#define XU_TXRTS_REQ_OFF       0x6008UL   /* shim sole writer */
#define XU_TXRTS_DONE_OFF      0x6010UL   /* daemon sole writer, held until REQ drops */
#define XU_ETXST_OFF           0x6018UL   /* shim writes, daemon reads */
#define XU_ETXLEN_OFF          0x6020UL   /* shim writes, daemon reads */
#define XU_ERXST_OFF           0x6028UL   /* shim writes once at init */
#define XU_ERXHEAD_OFF         0x6030UL   /* daemon's own monotonic frame-enqueued count; shim derives PKTCNT locally */
#define XU_ERXTAIL_OFF         0x6038UL   /* shim writes, daemon reads */
#define XU_MAC_ADDR_OFF        0x6040UL   /* daemon writes once at start; low 48 bits */
#define XU_MAC_VALID_OFF       0x6048UL   /* daemon writes; 0 until MAC_ADDR is valid */
