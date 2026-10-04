// tu58.h -- PDP2011: TU58 DECtape II emulator (tu58fs) on the serial port
// (support/pdp2011/PLAN-host-compat.md, 3).
#ifndef PDP2011_TU58_H
#define PDP2011_TU58_H

// Call every main-loop pass. active = the PDP2011 core is loaded; when it is
// not, a running emulator is stopped. Runs tu58fs on /dev/ttyS1 while the
// OSD has Console = Virtual VT100 and Line 1 = TU58.
void pdp2011_tu58_poll(int active);

// Call just before Main restarts for another core (app_restart): the next
// Main may be one that knows nothing about the TU58, so unless the new core
// is a PDP2011 the emulator is stopped here (saving its cartridges).
void pdp2011_tu58_before_restart(const char *rbf_path);

#endif
