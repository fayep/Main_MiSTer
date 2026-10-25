// fssize.h -- PDP2011: how much of a disk image its file system uses
#ifndef PDP2011_FSSIZE_H
#define PDP2011_FSSIZE_H

#include <stdint.h>
#include "../../file_io.h"

// Size in bytes of the RSTS/E, RT-11 or Files-11 ODS-1 (RSX) file system on the image, as
// SIMH's autosize reads it; 0 when none is recognised. name gets a short description.
uint64_t pdp2011_fs_size(fileTYPE *f, char *name, int namelen);

#endif
