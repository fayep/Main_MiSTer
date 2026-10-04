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

// Mount hook for the TM11 slot: if `name` (relative to the MiSTer root, as
// user_io_file_mount gets it) is a TPC or E11 tape, convert it once into
// games/PDP2011/.converted/ and return 1 with that path in `out`; the copy is
// reused while the source's size and mtime are unchanged. Returns 0 to mount
// `name` as is (SIMH, unrecognised, or the conversion failed). Never writes
// the source.
int pdp2011_tape_prepare(const char *name, char *out, size_t outlen);

#endif
