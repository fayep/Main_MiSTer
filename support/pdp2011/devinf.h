// devinf.h -- PDP2011 ".inf" device manifests (map a controller's units to image files)
#pragma once
#include <stdint.h>
#include <stddef.h>

// true if slot has an active .inf (unit routing in effect)
int pdp2011_inf_active(int slot);
// the unit an LBA addresses on this slot, or -1 if no .inf
int pdp2011_inf_unit(int slot, uint64_t lba);
// slot's per-unit stride in 512-byte blocks (0 if no .inf)
uint32_t pdp2011_inf_stride(int slot);

// resolve unit 0's image path from a manifest (for the normal mount); 0 if none named
int pdp2011_inf_unit0(const char *inf_path, char *out, size_t n);
// open units 1-3 of a slot from its manifest; stride in 512-byte blocks
void pdp2011_inf_mount(int slot, const char *inf_path, uint32_t stride_blocks, int writable);
void pdp2011_inf_unmount(int slot);

// serve a unit>=1 read/write (offset is bytes within that unit's pack); always handled (1)
int pdp2011_inf_read(int slot, int unit, uint64_t off_in_unit, uint8_t *buf, uint32_t len);
int pdp2011_inf_write(int slot, int unit, uint64_t off_in_unit, const uint8_t *buf, uint32_t len);
