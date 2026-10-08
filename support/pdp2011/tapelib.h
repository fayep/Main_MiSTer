// tapelib.h -- PDP2011: a tape library the guest drives over the XU.
//
// The guest (RSTS/E TLOAD/TDIR, rsts-tcpip src/tload.c) sends frames of
// Ethernet type 88-B5 (IEEE local experimental) to the library address
// 02-50-44-50-54-4C; xu_net hands them here instead of to the network and
// queues the reply back to the guest. The library is the core's home
// directory (games/PDP2011): .tap and .tpc images, mounted on the TM11.
//
// Payload (after the 14-byte header), multi-byte fields big-endian:
//   0  "TLIB"
//   4  op: 1 load (data = file name), 2 unload, 3 directory (data = 2-byte
//      first index), 4 status; the reply has op | 0x80
//   5  reply status (TL_*)
//   6  sequence number, echoed
//   8  data length, then the data
// Replies: load and status carry "name\0format"; directory carries the
// total count (2), the count here (2), then the names, NUL-separated.
#ifndef PDP2011_TAPELIB_H
#define PDP2011_TAPELIB_H

#include <stdint.h>

#define TL_OK       0
#define TL_NOFILE   1   // no such tape in the library
#define TL_BADNAME  2   // not a plain .tap/.tpc name
#define TL_FAILED   3   // the image would not mount
#define TL_BADOP    4
#define TL_EMPTY    5   // status: no tape mounted

// A frame the guest sent: if it is a library request, build the reply in
// reply (at least 1514 bytes) and return its length; otherwise return 0.
int pdp2011_tapelib_frame(const uint8_t *f, int len, uint8_t *reply);

#endif
