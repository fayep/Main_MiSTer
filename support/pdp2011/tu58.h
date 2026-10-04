// tu58.h -- PDP2011: TU58 DECtape II emulator (tu58fs) on the serial port
// (support/pdp2011/PLAN-host-compat.md, 3).
#ifndef PDP2011_TU58_H
#define PDP2011_TU58_H

// Call every main-loop pass. active = the PDP2011 core is loaded; when it is
// not, a running emulator is stopped. Runs tu58fs on /dev/ttyS1 while the
// OSD has Console = Virtual VT100 and Line 1 = TU58.
void pdp2011_tu58_poll(int active);

#endif
