// diskcompat.h -- PDP2011: disk images found in the wild
// (support/pdp2011/PLAN-host-compat.md, 2).
#ifndef PDP2011_DISKCOMPAT_H
#define PDP2011_DISKCOMPAT_H

#include <stdint.h>
#include "../../file_io.h"

// Slots: 0 RK, 1 RL, 2 RH. Call on every mount of a disk slot: for RL and RH it
// chooses the drive from the pack's extent (file system, else file size).
void pdp2011_disk_mounted(int slot, fileTYPE *f);

// The image size to tell the core for this slot: the chosen drive's nominal
// size for RL and RH (the core sets the drive type from it), else the file size.
uint64_t pdp2011_disk_report_size(int slot, fileTYPE *f);

// Write hook. Returns 1 when it has dealt with the write (grown the image,
// or dropped a write that would only grow a short RL image for its
// bad-sector table); 0 to let the normal path write it.
int pdp2011_disk_write(int slot, fileTYPE *f, uint64_t off, const uint8_t *buf, uint32_t len);

// Read hook, after `len` bytes at `off` have been read into buf (zeros past
// the end of the image): on an RL image with nothing in the bad-sector
// area, that area reads as an empty DEC STD 144 table.
void pdp2011_disk_read_fixup(int slot, fileTYPE *f, uint64_t off, uint8_t *buf, uint32_t len);

#endif
