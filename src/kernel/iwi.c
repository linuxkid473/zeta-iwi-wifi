/*
 * iwi - kernel driver for the Intel PRO/Wireless 2200BG / 2915ABG (PCI 8086:4220/4221/4223/4224)
 *       for BeOS ZETA (BeOS R5 style driver API, api_version 2).
 *
 * The driver is deliberately thin.  It owns the hardware and nothing else:
 *   /dev/misc/iwi/0  control face, used by the userland daemon `iwid` (ioctls + raw 802.11 frames)
 *   /dev/net/iwi/0   BONE face: an ordinary "Ethernet" device (ETHER_* ioctls, whole frames)
 * The card's firmware does scanning/authentication/association; WPA2 (EAPOL handshake, CCMP) lives in `iwid`.
 * The driver is polled: no interrupt handler is installed, the interrupt mask stays 0.
 * init_driver() only scans the PCI bus, so a broken card can never hang the boot.
 *
 * Copyright (c) 2026 Vihaan Nathan.  ISC licence, see LICENSE.
 * Register layout, firmware load sequence and descriptor formats follow the OpenBSD if_iwi.c / if_iwireg.h driver
 * (Copyright (c) 2004-2008 Damien Bergamini, ISC licence).  The firmware error log layout is from Linux ipw2200.
 * Intel's firmware is NOT part of this project (see docs/05-firmware.md).
 */
#include <KernelExport.h>
#include <Drivers.h>
#include <PCI.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int32 api_version = B_CUR_DRIVER_API_VERSION;

#define DEVNAME		"misc/iwi/0"
#define INTEL		0x8086

#define CSR_INTR		0x0008
#define CSR_INTR_MASK		0x000c
#define CSR_INDIRECT_ADDR	0x0010
#define CSR_INDIRECT_DATA	0x0014
#define CSR_AUTOINC_ADDR	0x0018
#define CSR_AUTOINC_DATA	0x001c
#define CSR_RST			0x0020
#define CSR_CTL			0x0024
#define CSR_IO			0x0030
#define CSR_READ_INT		0x0ff4

#define RST_PRINCETON_RESET	0x00000001
#define RST_SW_RESET		0x00000080
#define RST_MASTER_DISABLED	0x00000100
#define RST_STOP_MASTER		0x00000200
#define CTL_CLOCK_READY		0x00000001
#define CTL_INIT		0x00000004
#define READ_INT_INIT_HOST	0x20000000

#define MEM_EEPROM_CTL		0x00300040
#define EEPROM_C		(1 << 0)
#define EEPROM_S		(1 << 1)
#define EEPROM_D		(1 << 2)
#define EEPROM_Q		(1 << 4)
#define EEPROM_MAC		0x21

static pci_module_info*	gPci = NULL;
static pci_info		gInfo;
static bool		gFound = false;
static sem_id		gLock = -1;
static char*		gNames[3] = { DEVNAME, "net/iwi/0", NULL };
static volatile uint8*	gRegs = NULL;
static area_id		gArea = -1;

#define MEM_EVENT_CTL		0x00300004
#define INTR_FW_INITED		0x01000000
#define INTR_FATAL_ERROR	0x40000000
#define INTR_PARITY_ERROR	0x80000000
#define CB_DEFAULT_CTL		0x8cea0000
#define CB_MAXDATALEN		8191
#define FW_MAX			0x30000		/* 192 KB, firmware images are ~191 KB */
#define RING_AREA_SIZE		0x24000

static area_id		gBlobArea = -1;		/* firmware file as uploaded by iwid (IWI_IOC_FWCHUNK) */
static uint8*		gBlob = NULL;
static size_t		gBlobLen = 0;
static area_id		gDmaArea = -1;		/* contiguous staging area for one image */
static uint8*		gDma = NULL;
static uint32		gDmaPhys = 0;
static area_id		gRingArea = -1;		/* contiguous: command ring, tx rings, rx buffers */
static uint8*		gRing = NULL;
static uint32		gRingPhys = 0;

static uint32
csr_read(uint32 reg)
{
	return *(volatile uint32*)(gRegs + reg);
}

static void
csr_write(uint32 reg, uint32 val)
{
	*(volatile uint32*)(gRegs + reg) = val;
}

static uint32
mem_read(uint32 addr)
{
	csr_write(CSR_INDIRECT_ADDR, addr);
	return csr_read(CSR_INDIRECT_DATA);
}

static void
mem_write(uint32 addr, uint32 val)
{
	csr_write(CSR_INDIRECT_ADDR, addr);
	csr_write(CSR_INDIRECT_DATA, val);
}

static uint8
mem_read1(uint32 addr)
{
	csr_write(CSR_INDIRECT_ADDR, addr);
	return *(volatile uint8*)(gRegs + CSR_INDIRECT_DATA);
}

static void
mem_write1(uint32 addr, uint8 val)
{
	csr_write(CSR_INDIRECT_ADDR, addr);
	*(volatile uint8*)(gRegs + CSR_INDIRECT_DATA) = val;
}

static void
mem_write2(uint32 addr, uint16 val)
{
	csr_write(CSR_INDIRECT_ADDR, addr);
	*(volatile uint16*)(gRegs + CSR_INDIRECT_DATA) = val;
}

static void
eeprom_ctl(uint32 val)
{
	mem_write(MEM_EEPROM_CTL, val);
	spin(1);
}

static uint16
read_prom_word(uint8 addr)
{
	uint32 tmp;
	uint16 val = 0;
	int n;

	eeprom_ctl(0);
	eeprom_ctl(EEPROM_S);
	eeprom_ctl(EEPROM_S | EEPROM_C);
	eeprom_ctl(EEPROM_S);

	eeprom_ctl(EEPROM_S | EEPROM_D);
	eeprom_ctl(EEPROM_S | EEPROM_D | EEPROM_C);

	eeprom_ctl(EEPROM_S | EEPROM_D);
	eeprom_ctl(EEPROM_S | EEPROM_D | EEPROM_C);
	eeprom_ctl(EEPROM_S);
	eeprom_ctl(EEPROM_S | EEPROM_C);

	for (n = 7; n >= 0; n--) {
		eeprom_ctl(EEPROM_S | (((addr >> n) & 1) << 2));
		eeprom_ctl(EEPROM_S | (((addr >> n) & 1) << 2) | EEPROM_C);
	}

	eeprom_ctl(EEPROM_S);

	for (n = 15; n >= 0; n--) {
		eeprom_ctl(EEPROM_S | EEPROM_C);
		eeprom_ctl(EEPROM_S);
		tmp = mem_read(MEM_EEPROM_CTL);
		val |= ((tmp & EEPROM_Q) >> 4) << n;
	}

	eeprom_ctl(0);
	eeprom_ctl(EEPROM_S);
	eeprom_ctl(0);
	eeprom_ctl(EEPROM_C);
	return val;
}

static status_t
alloc_contig(const char* name, size_t size, area_id* area, uint8** virt, uint32* phys)
{
	physical_entry pe;

	*area = create_area(name, (void**)virt, B_ANY_KERNEL_ADDRESS, size,
		B_CONTIGUOUS, B_READ_AREA | B_WRITE_AREA);
	if (*area < 0)
		return *area;
	memset(*virt, 0, size);
	if (get_memory_map(*virt, size, &pe, 1) < B_OK) {
		delete_area(*area);
		*area = -1;
		return B_ERROR;
	}
	*phys = (uint32)pe.address;
	return B_OK;
}

static uint32
le32(const uint8* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

/* ucode_prep: stop bus mastering only (what iwi_load_ucode does inline) */
static int
stop_master_only(void)
{
	int ntries;
	uint32 tmp;

	tmp = csr_read(CSR_RST);
	csr_write(CSR_RST, tmp | RST_STOP_MASTER);
	for (ntries = 0; ntries < 5; ntries++) {
		if (csr_read(CSR_RST) & RST_MASTER_DISABLED)
			return 0;
		spin(10);
	}
	return -1;
}

/* iwi_stop_master: no interrupts, stop bus mastering, then hold the on-board CPU in reset */
static int
stop_master(void)
{
	int r;
	uint32 tmp;

	csr_write(CSR_INTR_MASK, 0);
	r = stop_master_only();
	tmp = csr_read(CSR_RST);
	csr_write(CSR_RST, tmp | RST_PRINCETON_RESET);
	return r;
}

static int
load_ucode(const uint8* data, int size)
{
	int ntries, i;
	uint32 tmp;
	const uint8* p;

	if (stop_master_only() < 0)
		return -1;

	mem_write(0x3000e0, 0x80000000);
	spin(5000);
	tmp = csr_read(CSR_RST);
	csr_write(CSR_RST, tmp & ~RST_PRINCETON_RESET);
	spin(5000);
	mem_write(0x3000e0, 0);
	spin(1000);
	mem_write(MEM_EVENT_CTL, 1);
	spin(1000);
	mem_write(MEM_EVENT_CTL, 0);
	spin(1000);
	mem_write1(0x200000, 0x00);
	mem_write1(0x200000, 0x40);
	spin(1000);

	/* adapter is buggy, the address must be set for each word */
	for (p = data; size > 0; p += 2, size -= 2)
		mem_write2(0x200010, p[0] | (p[1] << 8));

	mem_write1(0x200000, 0x00);
	mem_write1(0x200000, 0x80);

	for (ntries = 0; ntries < 100; ntries++) {
		if (mem_read1(0x200000) & 1)
			break;
		spin(100);
	}
	if (ntries == 100)
		return -2;

	for (i = 0; i < 7; i++)
		(void)mem_read(0x200004);
	mem_write1(0x200000, 0x00);
	return 0;
}

/* copy the image into the staging area, give the card command blocks that make it DMA the image in */
static int
load_firmware(const uint8* data, int size, uint32* intr_seen)
{
	const uint8 *p, *end;
	uint32 sentinel, tmp, ctl, src, dst, sum, len, mlen;
	int ntries;

	if (size > FW_MAX)
		return -10;
	memcpy(gDma, data, size);

	mem_write(0x3000a0, 0x27000);

	src = gDmaPhys;
	p = gDma;
	end = p + size;
	csr_write(CSR_AUTOINC_ADDR, 0x27000);

	while (p < end) {
		dst = le32(p); p += 4; src += 4;
		len = le32(p); p += 4; src += 4;
		p += len;

		while (len > 0) {
			mlen = len < CB_MAXDATALEN ? len : CB_MAXDATALEN;
			ctl = CB_DEFAULT_CTL | mlen;
			sum = ctl ^ src ^ dst;
			csr_write(CSR_AUTOINC_DATA, ctl);
			csr_write(CSR_AUTOINC_DATA, src);
			csr_write(CSR_AUTOINC_DATA, dst);
			csr_write(CSR_AUTOINC_DATA, sum);
			src += mlen;
			dst += mlen;
			len -= mlen;
		}
	}

	sentinel = csr_read(CSR_AUTOINC_ADDR);
	csr_write(CSR_AUTOINC_DATA, 0);

	tmp = csr_read(CSR_RST);
	tmp &= ~(RST_MASTER_DISABLED | RST_STOP_MASTER);
	csr_write(CSR_RST, tmp);

	mem_write(0x3000a4, 0x540100);

	for (ntries = 0; ntries < 400; ntries++) {
		if (mem_read(0x3000d0) >= sentinel)
			break;
		spin(100);
	}
	if (ntries == 400)
		return -11;

	mem_write(0x3000a4, 0x540c00);

	/* the BSD driver enables interrupts here; we leave the mask at 0 (no handler installed) and poll INTR */
	csr_write(CSR_RST, 0);
	tmp = csr_read(CSR_CTL);
	csr_write(CSR_CTL, tmp | 2);	/* IWI_CTL_ALLOW_STANDBY */

	*intr_seen = 0;
	for (ntries = 0; ntries < 1000; ntries++) {
		tmp = csr_read(CSR_INTR);
		if (tmp != 0 && tmp != 0xffffffff) {
			*intr_seen |= tmp;
			csr_write(CSR_INTR, tmp);	/* acknowledge */
		}
		if (*intr_seen & (INTR_FW_INITED | INTR_FATAL_ERROR | INTR_PARITY_ERROR))
			break;
		spin(1000);
	}
	if (*intr_seen & (INTR_FATAL_ERROR | INTR_PARITY_ERROR))
		return -12;
	if (!(*intr_seen & INTR_FW_INITED))
		return -13;
	return 0;
}


/* ---------------- command ring, RX and TX (polled; no interrupt handler is installed) ---------------- */

#define CMD_DESC_SIZE		128
#define CMD_RING_COUNT		16
#define RX_COUNT		32
#define RX_BUF_SIZE		2048
#define RX_BUF_OFFSET		0x11000
#define TX_RING_OFFSET		0x1000
#define TX_DESC_SIZE		128
#define TX_RING_COUNT		64
#define TX_BUF_SIZE		2048
#define TX_AREA_SIZE		(TX_RING_COUNT * TX_BUF_SIZE)

#define CSR_CMD_RIDX		0x0280
#define CSR_CMD_WIDX		0x0f80
#define CSR_TX_RIDX0		0x0284
#define CSR_TX_WIDX0		0x0f84
#define CSR_RX_RIDX		0x02a0
#define CSR_RX_WIDX		0x0fa0

#define HDR_TYPE_COMMAND	1
#define HDR_TYPE_NOTIF		3
#define HDR_TYPE_FRAME		9
#define HDR_FLAG_IRQ		0x04

#define IWI_IOC_FWCHUNK		20001
#define IWI_IOC_UP		20002
#define IWI_IOC_CMD		20003
#define IWI_IOC_DOWN		20004
#define IWI_IOC_STATUS		20007

struct iwi_fwchunk { unsigned long offset; unsigned long len; unsigned char data[4096]; };
struct iwi_up { long result; unsigned char mac[6]; unsigned char pad[2]; };
struct iwi_status { unsigned long v[48]; };
struct iwi_cmdio { unsigned char type; unsigned char len; unsigned char pad[2]; long result; unsigned char data[120]; };

static bool		gUp = false;
static area_id		gTxArea = -1;
static uint8*		gTxBuf = NULL;
static uint32		gTxPhys = 0;
static uint32		gCmdCur = 0;
static uint32		gRxCur = 0;
static uint32		gTxCur = 0;
static bool		gMacKnown = false;
static uint8		gMacCur[6];

static uint32		gIntrSeen = 0;

static void
ack_intr(void)
{
	uint32 intr = csr_read(CSR_INTR);

	if (intr != 0 && intr != 0xffffffff) {
		gIntrSeen |= intr;
		csr_write(CSR_INTR, intr);
	}
}

static int
send_cmd(uint8 type, const void* data, int len)
{
	uint8* d = gRing + gCmdCur * CMD_DESC_SIZE;
	int ntries;
	uint32 next = (gCmdCur + 1) % CMD_RING_COUNT;

	memset(d, 0, CMD_DESC_SIZE);
	d[0] = HDR_TYPE_COMMAND;
	d[2] = HDR_FLAG_IRQ;
	d[4] = type;
	d[5] = len;
	if (len > 0)
		memcpy(d + 8, data, len);
	gCmdCur = next;
	csr_write(CSR_CMD_WIDX, next);

	for (ntries = 0; ntries < 1000; ntries++) {
		ack_intr();
		if (csr_read(CSR_CMD_RIDX) == next)
			return 0;
		spin(1000);
	}
	return -1;
}

/* map the register window, reset the card to a known state and read the MAC address from the EEPROM */
static long
hw_reset(uint8 mac[6])
{
	uint32 base, size, tmp;
	uint16 cmd, w;
	uint8 m[6];
	int ntries, i;

	base = gInfo.u.h0.base_registers[0];
	size = gInfo.u.h0.base_register_sizes[0];
	cmd = gPci->read_pci_config(gInfo.bus, gInfo.device, gInfo.function, PCI_command, 2);
	gPci->write_pci_config(gInfo.bus, gInfo.device, gInfo.function, PCI_command, 2,
		cmd | PCI_command_memory | PCI_command_master);
	tmp = gPci->read_pci_config(gInfo.bus, gInfo.device, gInfo.function, 0x40, 4);
	gPci->write_pci_config(gInfo.bus, gInfo.device, gInfo.function, 0x40, 4, tmp & ~0x0000ff00UL);
	if (base == 0 || size == 0)
		return -4;
	if (gArea < 0) {
		gArea = map_physical_memory("iwi regs", (void*)base, 4096,
			B_ANY_KERNEL_ADDRESS, B_READ_AREA | B_WRITE_AREA, (void**)&gRegs);
		if (gArea < 0)
			return -5;
	}

	/* iwi_reset */
	stop_master();
	tmp = csr_read(CSR_CTL);
	csr_write(CSR_CTL, tmp | CTL_INIT);
	csr_write(CSR_READ_INT, READ_INT_INIT_HOST);
	for (ntries = 0; ntries < 1000; ntries++) {
		if (csr_read(CSR_CTL) & CTL_CLOCK_READY)
			break;
		spin(200);
	}
	if (ntries == 1000)
		return -6;
	tmp = csr_read(CSR_RST);
	csr_write(CSR_RST, tmp | RST_SW_RESET);
	spin(10);
	tmp = csr_read(CSR_CTL);
	csr_write(CSR_CTL, tmp | CTL_INIT);
	csr_write(CSR_AUTOINC_ADDR, 0);
	for (i = 0; i < 0xc000; i++)
		csr_write(CSR_AUTOINC_DATA, 0);

	w = read_prom_word(EEPROM_MAC + 0);
	m[0] = w & 0xff; m[1] = w >> 8;
	w = read_prom_word(EEPROM_MAC + 1);
	m[2] = w & 0xff; m[3] = w >> 8;
	w = read_prom_word(EEPROM_MAC + 2);
	m[4] = w & 0xff; m[5] = w >> 8;
	memcpy(mac, m, 6);
	gMacKnown = true;
	memcpy(gMacCur, m, 6);
	return 0;
}

static long
do_up(uint8 mac[6])
{
	uint32 base, size, tmp, seen;
	uint16 cmd, w;
	uint8 m[6];
	int ntries, i, ac;
	uint32 bootsz, ucodesz, mainsz;
	const uint8* fw;
	long r;

	gUp = false;
	gIntrSeen = 0;
	if (!gFound)
		return -1;
	if (gBlob == NULL || gBlobLen < 16 || gBlob[2] < 3)
		return -2;
	bootsz = le32(gBlob + 4);
	ucodesz = le32(gBlob + 8);
	mainsz = le32(gBlob + 12);
	if (gBlobLen < 16 + bootsz + ucodesz + mainsz || bootsz == 0 || ucodesz == 0 || mainsz == 0)
		return -3;

	r = hw_reset(mac);
	if (r != 0)
		return r;

	if (gDmaArea < 0 && alloc_contig("iwi fw dma", FW_MAX, &gDmaArea, &gDma, &gDmaPhys) < B_OK)
		return -7;
	if (gRingArea < 0 && alloc_contig("iwi rings", RING_AREA_SIZE, &gRingArea, &gRing, &gRingPhys) < B_OK)
		return -7;
	if (gTxArea < 0 && alloc_contig("iwi tx bufs", TX_AREA_SIZE, &gTxArea, &gTxBuf, &gTxPhys) < B_OK)
		return -7;
	memset(gRing, 0, RING_AREA_SIZE);
	gCmdCur = gRxCur = gTxCur = 0;

	fw = gBlob + 16;
	r = load_firmware(fw, bootsz, &seen);
	if (r != 0)
		return -20 + r;
	fw = gBlob + 16 + bootsz;
	r = load_ucode(fw, ucodesz);
	if (r != 0)
		return -30 + r;
	stop_master();

	csr_write(0x0200, gRingPhys);
	csr_write(0x0204, CMD_RING_COUNT);
	csr_write(CSR_CMD_WIDX, 0);
	for (ac = 0; ac < 4; ac++) {
		csr_write(0x0208 + ac * 8, gRingPhys + TX_RING_OFFSET + ac * 0x4000);
		csr_write(0x020c + ac * 8, TX_RING_COUNT);
		csr_write(0x0f84 + ac * 4, 0);
	}
	for (i = 0; i < RX_COUNT; i++)
		csr_write(0x0500 + i * 4, gRingPhys + RX_BUF_OFFSET + i * RX_BUF_SIZE);
	csr_write(CSR_RX_WIDX, RX_COUNT - 1);

	fw = gBlob + 16 + bootsz + ucodesz;
	r = load_firmware(fw, mainsz, &seen);
	if (r != 0)
		return -40 + r;
	gUp = true;
	return 0;
}

/* ---- BONE face support: two queues of Ethernet frames; the daemon moves them to and from the air ---- */

#define NET_Q			64
#define NET_FRAME		1536
#define IWI_IOC_NETRX		20005	/* daemon -> stack: struct iwi_netbuf */
#define IWI_IOC_NETTX		20006	/* stack -> daemon: struct iwi_netbuf (len 0 = nothing waiting) */

struct iwi_netbuf { long len; unsigned char data[NET_FRAME]; };

typedef struct {
	int			head;
	int			count;
	struct iwi_netbuf	f[NET_Q];
} netq;

static netq		gToStack, gFromStack;
static sem_id		gNetRxSem = -1;
static bool		gNetNonblock = false;
static bool		gNetClosing = false;

/* caller holds gLock */
static bool
q_push(netq* q, const uint8* data, int len)
{
	int slot;

	if (q->count == NET_Q)
		return false;
	slot = (q->head + q->count) % NET_Q;
	memcpy(q->f[slot].data, data, len);
	q->f[slot].len = len;
	q->count++;
	return true;
}

/* caller holds gLock; returns the frame length or 0 if empty */
static int
q_pop(netq* q, uint8* out, int max)
{
	int n;

	if (q->count == 0)
		return 0;
	n = q->f[q->head].len;
	if (n > max)
		n = max;
	memcpy(out, q->f[q->head].data, n);
	q->head = (q->head + 1) % NET_Q;
	q->count--;
	return n;
}

/* ---- device hooks ---- */

static status_t
iwi_open(const char* name, uint32 flags, void** cookie)
{
	*cookie = NULL;
	return B_OK;
}

static status_t
iwi_close(void* cookie)
{
	return B_OK;
}

static status_t
iwi_free(void* cookie)
{
	return B_OK;
}

static status_t
iwi_control(void* cookie, uint32 op, void* buf, size_t len)
{
	status_t status = B_OK;

	acquire_sem(gLock);
	switch (op) {
		case IWI_IOC_FWCHUNK:
		{
			struct iwi_fwchunk* c = (struct iwi_fwchunk*)buf;

			if (gBlobArea < 0) {
				gBlobArea = create_area("iwi fw blob", (void**)&gBlob, B_ANY_KERNEL_ADDRESS,
					FW_MAX, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
				if (gBlobArea < 0) {
					status = gBlobArea;
					break;
				}
			}
			if (c->len > 4096 || c->offset + c->len > FW_MAX) {
				status = B_ERROR;
				break;
			}
			memcpy(gBlob + c->offset, c->data, c->len);
			if (c->offset + c->len > gBlobLen)
				gBlobLen = c->offset + c->len;
			break;
		}
		case IWI_IOC_UP:
		{
			struct iwi_up* u = (struct iwi_up*)buf;

			u->result = do_up(u->mac);
			break;
		}
		case IWI_IOC_CMD:
		{
			struct iwi_cmdio* c = (struct iwi_cmdio*)buf;

			if (!gUp || c->len > 120) {
				c->result = -100;
				break;
			}
			c->result = send_cmd(c->type, c->data, c->len);
			break;
		}
		case IWI_IOC_STATUS:
		{
			struct iwi_status* st = (struct iwi_status*)buf;

			if (!gUp) {
				memset(st, 0, sizeof(*st));
				break;
			}
			st->v[0] = gIntrSeen;
			st->v[1] = csr_read(CSR_INTR);
			st->v[2] = csr_read(CSR_RST);
			st->v[3] = csr_read(CSR_CTL);
			st->v[4] = csr_read(CSR_IO);
			st->v[5] = csr_read(CSR_CMD_RIDX);
			st->v[6] = csr_read(CSR_RX_RIDX);
			st->v[7] = gRxCur;
			st->v[8] = csr_read(CSR_TX_RIDX0);
			st->v[9] = gTxCur;
			st->v[10] = csr_read(CSR_RX_WIDX);
			st->v[11] = csr_read(CSR_INTR_MASK);
			{
				/* firmware error log (as in the Linux ipw2200 driver): pointer at register 0x610,
				   then in NIC memory: count, followed by 7-word elements (desc time blink1 blink2 link1 link2 data) */
				uint32 base = csr_read(0x610), n = 0, k, j;

				st->v[12] = base;
				if (base != 0 && base < 0x400000 && (base & 3) == 0) {
					n = mem_read(base);
					st->v[13] = n;
					if (n > 4)
						n = 4;
					for (k = 0; k < n; k++)
						for (j = 0; j < 7; j++)
							st->v[14 + k * 7 + j] = mem_read(base + 4 + (k * 7 + j) * 4);
				}
			}
			break;
		}
		case IWI_IOC_NETRX:
		{
			struct iwi_netbuf* nb = (struct iwi_netbuf*)buf;

			if (nb->len >= 14 && nb->len <= 1514 && q_push(&gToStack, nb->data, nb->len))
				release_sem(gNetRxSem);
			break;
		}
		case IWI_IOC_NETTX:
		{
			struct iwi_netbuf* nb = (struct iwi_netbuf*)buf;

			nb->len = q_pop(&gFromStack, nb->data, NET_FRAME);
			break;
		}
		case IWI_IOC_DOWN:
			if (gUp) {
				send_cmd(33, NULL, 0);		/* DISABLE */
				csr_write(CSR_INTR_MASK, 0);
				stop_master();
				gUp = false;
			}
			break;
		default:
			status = B_ERROR;
	}
	release_sem(gLock);
	return status;
}

static status_t
iwi_read(void* cookie, off_t pos, void* buf, size_t* len)
{
	const uint8* b;
	size_t n;
	uint32 hw;

	acquire_sem(gLock);
	if (!gUp) {
		*len = 0;
		release_sem(gLock);
		return B_ERROR;
	}
	ack_intr();
	hw = csr_read(CSR_RX_RIDX);
	if (gRxCur == hw) {
		*len = 0;
		release_sem(gLock);
		return B_OK;
	}
	b = gRing + RX_BUF_OFFSET + gRxCur * RX_BUF_SIZE;
	if (b[0] == HDR_TYPE_FRAME) {
		n = 4 + 24 + (b[4 + 22] | (b[4 + 23] << 8));
	} else if (b[0] == HDR_TYPE_NOTIF) {
		n = 4 + 12 + (b[4 + 10] | (b[4 + 11] << 8));
	} else
		n = 32;
	if (n > RX_BUF_SIZE)
		n = RX_BUF_SIZE;
	if (n > *len)
		n = *len;
	memcpy(buf, b, n);
	*len = n;
	gRxCur = (gRxCur + 1) % RX_COUNT;
	csr_write(CSR_RX_WIDX, gRxCur == 0 ? RX_COUNT - 1 : gRxCur - 1);
	release_sem(gLock);
	return B_OK;
}

/* one 802.11 data frame per write() */
static status_t
iwi_write(void* cookie, off_t pos, const void* buf, size_t* len)
{
	const uint8* f = (const uint8*)buf;
	uint8* d;
	int hdrlen, plen, idx;
	size_t n = *len;
	bool qos;

	acquire_sem(gLock);
	if (!gUp || n < 24 || n > TX_BUF_SIZE - 64) {
		*len = 0;
		release_sem(gLock);
		return B_ERROR;
	}
	qos = (f[0] & 0x8c) == 0x88;
	hdrlen = qos ? 26 : 24;
	if (n < (size_t)hdrlen) {
		*len = 0;
		release_sem(gLock);
		return B_ERROR;
	}
	plen = n - hdrlen;
	idx = gTxCur;
	if ((uint32)((idx + 1) % TX_RING_COUNT) == csr_read(CSR_TX_RIDX0)) {
		*len = 0;
		release_sem(gLock);
		return B_BUSY;
	}
	d = gRing + TX_RING_OFFSET + idx * TX_DESC_SIZE;
	memset(d, 0, TX_DESC_SIZE);
	d[0] = 0;				/* IWI_HDR_TYPE_DATA */
	d[2] = HDR_FLAG_IRQ;
	d[12] = 0x0b;				/* IWI_DATA_CMD_TX */
	d[14] = plen & 0xff;
	d[15] = plen >> 8;
	d[17] = 0x20 | ((f[4] & 1) ? 0 : 0x80);	/* NO_WEP (we encrypt), NEED_ACK for unicast */
	if (qos)
		d[18] = 0x10;			/* XFLAG_QOS */
	memcpy(d + 48, f, hdrlen);		/* desc->wh */
	d[88] = 1;				/* nseg */
	{
		uint32 pa = gTxPhys + idx * TX_BUF_SIZE;
		d[92] = pa & 0xff; d[93] = (pa >> 8) & 0xff; d[94] = (pa >> 16) & 0xff; d[95] = pa >> 24;
	}
	d[116] = plen & 0xff;			/* seg_len[0] */
	d[117] = plen >> 8;
	memcpy(gTxBuf + idx * TX_BUF_SIZE, f + hdrlen, plen);
	gTxCur = (idx + 1) % TX_RING_COUNT;
	csr_write(CSR_TX_WIDX0, gTxCur);
	ack_intr();
	release_sem(gLock);
	return B_OK;
}

static device_hooks gHooks = {
	iwi_open, iwi_close, iwi_free, iwi_control, iwi_read, iwi_write,
	NULL, NULL, NULL, NULL
};

/* ---- /dev/net/iwi/0: what the BONE network stack talks to ---- */

#define ETHER_GETADDR_OP	9999
#define ETHER_INIT_OP		10000
#define ETHER_NONBLOCK_OP	10001
#define ETHER_ADDMULTI_OP	10002
#define ETHER_REMMULTI_OP	10003
#define ETHER_SETPROMISC_OP	10004
#define ETHER_GETFRAMESIZE_OP	10005
#define ETHER_HASIOVECS_OP	10007
#define ETHER_GETIFTYPE_OP	10008

static status_t
net_open(const char* name, uint32 flags, void** cookie)
{
	*cookie = NULL;
	gNetClosing = false;
	return B_OK;
}

static status_t
net_close(void* cookie)
{
	gNetClosing = true;
	release_sem(gNetRxSem);		/* wake a blocked reader */
	return B_OK;
}

static status_t
net_free(void* cookie)
{
	return B_OK;
}

static status_t
net_control(void* cookie, uint32 op, void* buf, size_t len)
{
	switch (op) {
		case ETHER_GETADDR_OP:
		{
			uint8 mac[6];
			status_t status = B_OK;

			acquire_sem(gLock);
			if (!gMacKnown && !gUp && gFound) {
				if (hw_reset(mac) != 0)
					status = B_ERROR;
			}
			if (status == B_OK && gMacKnown)
				memcpy(buf, gMacCur, 6);
			else
				status = B_ERROR;
			release_sem(gLock);
			return status;
		}
		case ETHER_INIT_OP:
		case ETHER_ADDMULTI_OP:
		case ETHER_REMMULTI_OP:
		case ETHER_SETPROMISC_OP:
			return B_OK;
		case ETHER_NONBLOCK_OP:
			gNetNonblock = (*(int*)buf != 0);
			return B_OK;
		case ETHER_GETFRAMESIZE_OP:
			*(int*)buf = 1514;
			return B_OK;
		/* ETHER_HASIOVECS / GETIFTYPE / ADDTIMESTAMP are deliberately NOT answered: ZETA's own eepro100 driver
		   returns an error for them, and answering HASIOVECS made BONE write every frame in separate pieces */
	}
	return B_ERROR;
}

static status_t
net_read(void* cookie, off_t pos, void* buf, size_t* len)
{
	status_t status;
	int n;

	status = acquire_sem_etc(gNetRxSem, 1, gNetNonblock ? B_RELATIVE_TIMEOUT : B_CAN_INTERRUPT, 0);
	if (status != B_OK) {
		*len = 0;
		return gNetNonblock ? B_WOULD_BLOCK : status;
	}
	if (gNetClosing) {
		*len = 0;
		return B_INTERRUPTED;
	}
	acquire_sem(gLock);
	n = q_pop(&gToStack, (uint8*)buf, *len);
	release_sem(gLock);
	*len = n;
	return B_OK;
}

static status_t
net_write(void* cookie, off_t pos, const void* buf, size_t* len)
{
	if (*len < 14 || *len > 1514) {
		*len = 0;
		return B_ERROR;
	}
	acquire_sem(gLock);
	q_push(&gFromStack, (const uint8*)buf, *len);	/* a full queue drops the frame, like a busy NIC */
	release_sem(gLock);
	return B_OK;
}

static device_hooks gNetHooks = {
	net_open, net_close, net_free, net_control, net_read, net_write,
	NULL, NULL, NULL, NULL
};

/* ---- driver entry points ---- */

status_t
init_hardware(void)
{
	return B_OK;
}

status_t
init_driver(void)
{
	long i;
	status_t status;
	pci_info info;

	status = get_module(B_PCI_MODULE_NAME, (module_info**)&gPci);
	if (status < B_OK)
		return status;

	for (i = 0; gPci->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (info.vendor_id == INTEL
			&& (info.device_id == 0x4220 || info.device_id == 0x4221
				|| info.device_id == 0x4223 || info.device_id == 0x4224)) {
			gInfo = info;
			gFound = true;
			break;
		}
	}

	gLock = create_sem(1, "iwi lock");
	gNetRxSem = create_sem(0, "iwi net rx");
	if (gLock < 0 || gNetRxSem < 0) {
		put_module(B_PCI_MODULE_NAME);
		return B_NO_MEMORY;
	}
	dprintf("iwi: init_driver, card %s\n", gFound ? "found" : "not found");
	return B_OK;
}

void
uninit_driver(void)
{
	if (gUp) {
		csr_write(CSR_INTR_MASK, 0);
		stop_master();
		gUp = false;
	}
	if (gTxArea >= 0)
		delete_area(gTxArea);
	if (gDmaArea >= 0)
		delete_area(gDmaArea);
	if (gRingArea >= 0)
		delete_area(gRingArea);
	if (gBlobArea >= 0)
		delete_area(gBlobArea);
	if (gArea >= 0)
		delete_area(gArea);
	if (gLock >= 0)
		delete_sem(gLock);
	if (gNetRxSem >= 0)
		delete_sem(gNetRxSem);
	put_module(B_PCI_MODULE_NAME);
}

const char**
publish_devices(void)
{
	return (const char**)gNames;
}

device_hooks*
find_device(const char* name)
{
	if (strcmp(name, DEVNAME) == 0)
		return &gHooks;
	if (strcmp(name, "net/iwi/0") == 0)
		return &gNetHooks;
	return NULL;
}
