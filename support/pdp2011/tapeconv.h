// tapeconv.h -- PDP2011: recognise TPC / E11 tape images and convert them to
// the SIMH .tap format the core's TM11 reads (support/pdp2011/PLAN-host-compat.md, 1).
#ifndef PDP2011_TAPECONV_H
#define PDP2011_TAPECONV_H

#include <stddef.h>

enum tape_format { TAPE_UNKNOWN = 0, TAPE_SIMH, TAPE_TPC, TAPE_E11 };

// Walk the whole file and decide its format. SIMH wins when a file parses
// both ways.
enum tape_format tape_detect(const char *path);

// Convert src (TPC or E11) to SIMH format at dst: every record and tape mark
// carried over, then an end-of-medium marker. Streams; returns 1 on success.
int tape_convert(const char *src, const char *dst, enum tape_format fmt);

const char *tape_format_name(enum tape_format fmt);

#include <stdint.h>

// Streaming (support/pdp2011/PLAN-host-compat.md, 1): a TPC/E11 image in tape
// slot `slot` is served to the core as a SIMH stream, block by block, with no
// conversion and nothing written. attach() after the image is opened (any
// other format just detaches); read() fills len bytes from virtual offset off.
void pdp2011_tape_attach(int slot, const char *path, enum tape_format fmt);
void pdp2011_tape_detach(int slot);
int pdp2011_tape_streaming(int slot);
int pdp2011_tape_read(int slot, uint64_t off, uint8_t *buf, uint32_t len);

#endif
