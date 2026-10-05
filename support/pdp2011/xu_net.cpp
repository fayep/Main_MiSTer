// xu_net.cpp -- PDP2011: host networking for the XU (DEUNA), see xu_net.h.
//
// Modelled on support/next/next_enet.cpp (NeXT core): the NIC lives in the
// FPGA and only frames cross. The ring layout is the bridge's, byte offsets
// from 0x1FF00000, one writer per word:
//
//   0x0000 MAGIC "PDPXU001" fpga   0x0028 GUEST fpga: b63 valid, b55:48 mode
//   0x0008 TX_WPTR          fpga          (b0 prom, b1 all-multicast, b2 loop),
//   0x0010 TX_RPTR          arm           b47:0 mac
//   0x0018 RX_WPTR          arm    0x0030 HOST  arm: b63 valid, b47:0 mac
//   0x0020 RX_RPTR          fpga   0x0038 HEARTBEAT arm, +1 per poll
//   0x1000 TX slots 8 x 2048       0x5000 RX slots 8 x 2048
//
// slot: u64 header (bits 10:0 length), frame from +8. MAC byte i is bits
// 8i+7:8i of its word. Pointers are free-running; slot = pointer mod 8.
// The bridge adopts our pointers when it starts, so nothing is reset here.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "xu_net.h"
#include "../../user_io.h"
#include "../../shmem.h"

extern int  ethernet_open(const char *iface, int promiscuous);
extern void ethernet_close(void);
extern void ethernet_send(const uint8_t *frame, int len);
extern int  ethernet_recv_nb(uint8_t *buf, int maxlen);
extern int  ethernet_read_iface_mac(const char *iface, uint8_t *out);
extern int  ethernet_set_mac_filter(const uint8_t *mac);
extern void ethernet_clear_filter(void);
extern int  ethernet_iface_present(const char *iface);
extern int  ethernet_macvlan_create(const char *parent, const char *name, const uint8_t *mac);
extern void ethernet_macvlan_delete(const char *name);

#define XN_BASE        0x1FF00000UL
#define XN_SIZE        0x10000UL

#define XN_MAGIC_OFF   0x0000
#define XN_TXWPTR_OFF  0x0008
#define XN_TXRPTR_OFF  0x0010
#define XN_RXWPTR_OFF  0x0018
#define XN_RXRPTR_OFF  0x0020
#define XN_GUEST_OFF   0x0028
#define XN_HOST_OFF    0x0030
#define XN_HEART_OFF   0x0038
#define XN_TXSLOT_OFF  0x1000
#define XN_RXSLOT_OFF  0x5000
#define XN_SLOT_SIZE   0x800
#define XN_RING        8
#define XN_MAXLEN      2040

#define XN_MAGIC       0x3130305558504450ULL   // "PDPXU001"

#define XN_MODE_PROM   0x01
#define XN_MODE_ENAL   0x02
#define XN_MODE_LOOP   0x04

enum { NET_OFF, NET_ETH0, NET_ETH1, NET_MACVLAN, NET_TAP0 };
static const char *net_name[] = { "off", "eth0", "eth1", "macvlan", "tap0" };

#define MACVLAN_NAME   "pdp0"
#define MAX_FRAME      1600

static volatile uint8_t *mb = 0;
static int      cur_net = NET_OFF;
static int      link_open = 0;
static int      made_macvlan = 0;
static int      warned = 0;
static uint8_t  guest_mac[6];
static int      guest_mode = 0;
static int      guest_known = 0;
static uint64_t host_word = 0;

static inline uint64_t rd64(uint32_t off) { return *(volatile uint64_t *)(mb + off); }
static inline void wr64(uint32_t off, uint64_t v) { *(volatile uint64_t *)(mb + off) = v; }

// Ring pointers are 32-bit counters in the low half of their word, as the
// bridge keeps them. Never use the upper half: a word nobody has written yet
// holds whatever DDR3 powered up with, and one stray bit there makes the
// ring look permanently full.
static inline uint32_t rdptr(uint32_t off) { return (uint32_t)rd64(off); }
static inline void wrptr(uint32_t off, uint32_t v) { wr64(off, (uint64_t)v); }

static uint64_t mac_word(const uint8_t *m)
{
	uint64_t v = 0;
	for (int i = 0; i < 6; i++) v |= (uint64_t)m[i] << (8 * i);
	return v;
}

// The default address offered to the guest: DEC's 08-00-2B and the low
// three bytes of eth0's address, so each MiSTer has its own.
static void make_host_word(void)
{
	uint8_t e[6] = { 0, 0, 0, 0, 0, 1 };
	ethernet_read_iface_mac("eth0", e);
	uint8_t m[6] = { 0x08, 0x00, 0x2B, e[3], e[4], e[5] };
	host_word = (1ULL << 63) | mac_word(m);
	printf("[pdp2011-xu] default address %02X-%02X-%02X-%02X-%02X-%02X\n", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void close_link(void)
{
	if (link_open)
	{
		ethernet_clear_filter();
		ethernet_close();
		link_open = 0;
	}
	if (made_macvlan)
	{
		ethernet_macvlan_delete(MACVLAN_NAME);
		made_macvlan = 0;
	}
}

static void open_link(int net)
{
	const char *iface = 0;
	int promisc = 0;

	close_link();
	switch (net)
	{
	case NET_ETH0:
		iface = "eth0";
		promisc = 1;            // shared with Linux: promiscuous plus the guest MAC filter
		break;
	case NET_ETH1:
		iface = "eth1";
		promisc = 1;
		break;
	case NET_MACVLAN:
		if (!guest_known) return;   // the child is created with the guest's address
		if (!ethernet_macvlan_create("eth0", MACVLAN_NAME, guest_mac)) return;
		made_macvlan = 1;
		iface = MACVLAN_NAME;
		promisc = (guest_mode & (XN_MODE_PROM | XN_MODE_ENAL)) != 0;
		break;
	case NET_TAP0:
		iface = "tap0";
		break;
	default:
		return;
	}

	if (net != NET_MACVLAN && !ethernet_iface_present(iface))
	{
		if (!warned) printf("[pdp2011-xu] %s not present\n", iface);
		warned = 1;
		return;
	}
	if (!ethernet_open(iface, promisc))
	{
		close_link();
		return;
	}
	link_open = 1;
	warned = 0;

	uint8_t scratch[MAX_FRAME];
	while (ethernet_recv_nb(scratch, MAX_FRAME) > 0) ;

	if (net == NET_ETH0 && guest_known && !(guest_mode & XN_MODE_PROM))
		ethernet_set_mac_filter(guest_mac);
	printf("[pdp2011-xu] up on %s\n", iface);
}

// What the DEUNA would accept: its own address, broadcast and multicast
// (the multicast list is not applied: all of them pass), or everything when
// promiscuous. Our own transmissions seen again on a shared NIC are dropped.
static int wanted(const uint8_t *f)
{
	if (!guest_known) return 0;
	if (!memcmp(f + 6, guest_mac, 6)) return 0;
	if (guest_mode & XN_MODE_PROM) return 1;
	if (f[0] & 1) return 1;
	return !memcmp(f, guest_mac, 6);
}

// Received frames wait in two queues across polls: unicast to the guest,
// then broadcast/multicast. Unicast always goes into the ring first; the
// rest only while the ring keeps XN_UNI_HEADROOM slots free for unicast
// still to come, best effort (dropped when its queue is full). No
// ethertype filtering: DECnet (60-03), LAT and MOP must get through.
#define XN_UNIQ        64
#define XN_OTHQ        32
#define XN_UNI_HEADROOM 2

struct xn_frame { uint16_t len; uint8_t buf[MAX_FRAME]; };
struct xn_queue { int head, count, depth; struct xn_frame *f; };
static struct xn_frame uni_buf[XN_UNIQ], oth_buf[XN_OTHQ];
static struct xn_queue uniq = { 0, 0, XN_UNIQ, uni_buf }, othq = { 0, 0, XN_OTHQ, oth_buf };
static unsigned long drops_uni = 0, drops_oth = 0;

static int q_push(struct xn_queue *q, const uint8_t *f, int len)
{
	if (q->count == q->depth) return 0;
	struct xn_frame *e = &q->f[(q->head + q->count) % q->depth];
	e->len = (uint16_t)len;
	memcpy(e->buf, f, len);
	q->count++;
	return 1;
}

static void q_clear(void)
{
	uniq.head = uniq.count = 0;
	othq.head = othq.count = 0;
}

static uint32_t ring_used(void)
{
	return (uint32_t)(rdptr(XN_RXWPTR_OFF) - rdptr(XN_RXRPTR_OFF));
}

static void deliver(const uint8_t *f, int len)
{
	uint32_t rxw = rdptr(XN_RXWPTR_OFF);
	uint32_t rxr = rdptr(XN_RXRPTR_OFF);
	if ((uint32_t)(rxw - rxr) >= XN_RING) return;
	if (len > XN_MAXLEN) len = XN_MAXLEN;
	uint32_t slot = XN_RXSLOT_OFF + XN_SLOT_SIZE * (uint32_t)(rxw % XN_RING);
	memcpy((void *)(mb + slot + 8), f, len);
	__sync_synchronize();
	wr64(slot, (uint64_t)len);
	__sync_synchronize();
	wrptr(XN_RXWPTR_OFF, rxw + 1);
}

void pdp2011_xu_stop(void)
{
	close_link();
	if (mb)
	{
		wr64(XN_HOST_OFF, 0);
		shmem_unmap((void *)mb, XN_SIZE);
		mb = 0;
		printf("[pdp2011-xu] stopped\n");
	}
	cur_net = NET_OFF;
	guest_known = 0;
	q_clear();
}

void pdp2011_xu_poll(void)
{
	static uint8_t frame[MAX_FRAME];

	int net = (int)user_io_status_get("[27:25]");
	if (net < NET_OFF || net > NET_TAP0) net = NET_OFF;
	if (net == NET_OFF)
	{
		if (mb) pdp2011_xu_stop();
		return;
	}

	if (!mb)
	{
		mb = (volatile uint8_t *)shmem_map(XN_BASE, XN_SIZE);
		if (!mb)
		{
			printf("[pdp2011-xu] shmem_map failed\n");
			return;
		}
		make_host_word();
		printf("[pdp2011-xu] armed (%s)\n", net_name[net]);
	}

	wr64(XN_HOST_OFF, host_word);
	wr64(XN_HEART_OFF, rd64(XN_HEART_OFF) + 1);
	if (rd64(XN_MAGIC_OFF) != XN_MAGIC) return;    // bridge not running yet

	// the guest's address and mode
	uint64_t g = rd64(XN_GUEST_OFF);
	if (g >> 63)
	{
		uint8_t m[6];
		for (int i = 0; i < 6; i++) m[i] = (g >> (8 * i)) & 0xFF;
		int mode = (g >> 48) & 0xFF;
		if (!guest_known || memcmp(m, guest_mac, 6) || mode != guest_mode)
		{
			int mac_changed = !guest_known || memcmp(m, guest_mac, 6);
			memcpy(guest_mac, m, 6);
			guest_mode = mode;
			guest_known = 1;
			printf("[pdp2011-xu] guest %02X-%02X-%02X-%02X-%02X-%02X mode %02X\n",
				m[0], m[1], m[2], m[3], m[4], m[5], mode);
			if (link_open && (cur_net == NET_MACVLAN || (cur_net == NET_ETH0 && mac_changed)))
				open_link(cur_net);                  // re-create the child / re-apply the filter
			else if (link_open && cur_net == NET_ETH0)
			{
				if (mode & XN_MODE_PROM) ethernet_clear_filter();
				else ethernet_set_mac_filter(guest_mac);
			}
		}
	}

	// follow the OSD
	if (net != cur_net)
	{
		cur_net = net;
		open_link(net);
	}
	else if (!link_open)
	{
		static uint32_t retry = 0;
		if (!(retry++ & 255)) open_link(net);     // interface appeared, or macvlan now has its address
	}

	// transmit: everything the guest queued
	uint32_t txw = rdptr(XN_TXWPTR_OFF);
	uint32_t txr = rdptr(XN_TXRPTR_OFF);
	if ((uint32_t)(txw - txr) > XN_RING) txr = txw - XN_RING;
	while (txr != txw)
	{
		uint32_t slot = XN_TXSLOT_OFF + XN_SLOT_SIZE * (uint32_t)(txr % XN_RING);
		int len = (int)(rd64(slot) & 0x7FF);
		if (len >= 14 && len <= MAX_FRAME)
		{
			memcpy(frame, (const void *)(mb + slot + 8), len);
			if (guest_mode & XN_MODE_LOOP) deliver(frame, len);
			else if (link_open) ethernet_send(frame, len);
		}
		txr++;
	}
	__sync_synchronize();
	wrptr(XN_TXRPTR_OFF, txr);

	// receive: drain the socket every pass into the two queues
	if (!link_open) return;
	for (int n = 0; n < 256; n++)
	{
		int len = ethernet_recv_nb(frame, MAX_FRAME);
		if (len <= 0) break;
		if (len < 14 || (guest_mode & XN_MODE_LOOP) || !wanted(frame)) continue;
		if (frame[0] & 1) { if (!q_push(&othq, frame, len)) drops_oth++; }
		else if (!q_push(&uniq, frame, len)) drops_uni++;
	}

	// then the ring: unicast first, the rest into what unicast leaves
	while (uniq.count && ring_used() < XN_RING)
	{
		struct xn_frame *e = &uniq.f[uniq.head];
		deliver(e->buf, e->len);
		uniq.head = (uniq.head + 1) % uniq.depth; uniq.count--;
	}
	while (othq.count && !uniq.count && ring_used() < XN_RING - XN_UNI_HEADROOM)
	{
		struct xn_frame *e = &othq.f[othq.head];
		deliver(e->buf, e->len);
		othq.head = (othq.head + 1) % othq.depth; othq.count--;
	}

	static uint32_t stats_t = 0;
	if (!(++stats_t & 0xFFFF) && (drops_uni || drops_oth))
		printf("[pdp2011-xu] receive queue drops: unicast %lu, broadcast/multicast %lu\n", drops_uni, drops_oth);
}
