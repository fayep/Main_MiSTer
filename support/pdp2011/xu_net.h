// xu_net.h -- PDP2011: host networking for the XU (DEUNA). The core's
// frame port and DDR3 bridge (PDP2011_MiSTer rtl/xufp.vhd,
// rtl/xu_ddr_bridge.vhd, notes/xu-frames-plan.md) pass whole Ethernet
// frames through rings at ARM physical 0x1FF00000; this puts them on the
// interface chosen by OSD "Host network" (eth0, eth1, macvlan, tap0).
#ifndef PDP2011_XU_NET_H
#define PDP2011_XU_NET_H

// Call every main-loop pass while the PDP2011 core is loaded.
void pdp2011_xu_poll(void);

// Call on every core switch (user_io_init): closes the interface and
// removes the macvlan child, if any.
void pdp2011_xu_stop(void);

#endif
