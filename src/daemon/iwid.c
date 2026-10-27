/* iwid - userland WPA2-PSK (CCMP) supplicant for the iwi kernel driver, for BeOS / ZETA.

   The card's firmware does the 802.11 scan / authenticate / associate.  Everything else is done here:
   PBKDF2 PMK, the EAPOL 4-way handshake, and CCMP encryption / decryption of data frames (the driver hands us
   raw 802.11 frames).  Config file /boot/home/config/settings/iwi.conf (you create it yourself):
       ssid=YourNetworkName
       psk=YourPassphrase
   optional:  fw=/path/to/iwi-bss   gw=192.168.0.1   (gateway used for the ARP self-test)                         */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <OS.h>

#include "wpa.h"
#include "iwi_ioctl.h"

#define CONF_PATH	"/boot/home/config/settings/iwi.conf"
#define DEFAULT_FW	"/boot/home/ZetaWifi/iwi-bss"

#define CMD_ENABLE		2
#define CMD_SET_CONFIG		6
#define CMD_SET_ESSID		8
#define CMD_SET_MAC_ADDRESS	11
#define CMD_SET_RTS_THRESHOLD	15
#define CMD_SET_FRAG_THRESHOLD	16
#define CMD_SET_POWER_MODE	17
#define CMD_ASSOCIATE		21
#define CMD_SET_RATES		22
#define CMD_SCAN		26
#define CMD_SET_OPTIE		31
#define CMD_DISABLE		33
#define CMD_SET_RANDOM_SEED	34
#define CMD_SET_TX_POWER	35
#define CMD_SET_SENSITIVITY	42

#define ITEM_NOTIF		3
#define ITEM_FRAME		9
#define NOTIF_ASSOCIATION	10
#define NOTIF_AUTHENTICATION	11
#define NOTIF_SCAN_COMPLETE	13

#define MAX_AP			32

static int gFd = -1;
static u8 gMac[6];
static int gVerbose = 0;
static unsigned int gSeq = 0;
static int gBridge = 0, gIdle = 0;
static void report_card(const char* why);

/* ------------------------------ small helpers ------------------------------ */

static void
say(const char* fmt, ...)
{
	va_list ap;

	printf("[%6.2f] ", (double)system_time() / 1000000.0);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static void
hexdump(const char* tag, const u8* p, int n)
{
	int i;

	printf("          %s:", tag);
	for (i = 0; i < n; i++)
		printf("%s%02x", (i % 32 == 0 && i) ? "\n            " : " ", p[i]);
	printf("\n");
	fflush(stdout);
}

static void
mac_str(const u8* m, char* out)
{
	sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void
random_bytes(u8* out, int n)
{
	int fd = open("/dev/random", O_RDONLY);
	int got = 0;
	static unsigned int counter = 0;

	if (fd >= 0) {
		got = read(fd, out, n);
		close(fd);
	}
	if (got < n) {
		/* fallback: hash of time and a counter (weaker, but fine for a hobby supplicant) */
		u8 h[20], seed[16];
		int i;
		for (i = got; i < n; i += 20) {
			bigtime_t t = system_time();
			memcpy(seed, &t, 8);
			memcpy(seed + 8, &counter, 4);
			counter++;
			memcpy(seed + 12, &i, 4);
			sha1(seed, 16, h);
			memcpy(out + i, h, n - i < 20 ? n - i : 20);
		}
	}
}

/* ------------------------------ driver access ------------------------------ */

static int
fw_cmd(unsigned char type, const void* data, int len)
{
	struct iwi_cmdio c;

	memset(&c, 0, sizeof(c));
	c.type = type;
	c.len = len;
	if (len > 0)
		memcpy(c.data, data, len);
	if (ioctl(gFd, IWI_IOC_CMD, &c, sizeof(c)) < 0)
		return -1000;
	return (int)c.result;
}

static int
upload_firmware(const char* path)
{
	FILE* f = fopen(path, "rb");
	static struct iwi_fwchunk chunk;
	unsigned long off = 0;
	int n;

	if (f == NULL) {
		say("cannot open firmware %s", path);
		return -1;
	}
	for (;;) {
		n = fread(chunk.data, 1, sizeof(chunk.data), f);
		if (n <= 0)
			break;
		chunk.offset = off;
		chunk.len = n;
		if (ioctl(gFd, IWI_IOC_FWCHUNK, &chunk, sizeof(chunk)) < 0) {
			say("firmware upload failed at offset %lu", off);
			fclose(f);
			return -1;
		}
		off += n;
	}
	fclose(f);
	say("firmware: %lu bytes uploaded", off);
	return 0;
}

/* one received item from the driver; returns length (0 = none) */
static int gLogMgmt = 0;

static int
rx_item(u8* buf)
{
	int n = read(gFd, buf, 2048);

	if (n < 0)
		return 0;
	if (gLogMgmt) {
		static bigtime_t last = 0;
		static int items = 0, beacons = 0, datas = 0, notifs = 0;
		if (last == 0)
			last = system_time();
		if (n > 0) {
			items++;
			if (buf[0] == 3)
				notifs++;
			else if ((buf[28] & 0xfc) == 0x80)
				beacons++;
			else if ((buf[28] & 0x0c) == 0x08)
				datas++;
		}
		if (system_time() - last > 5000000) {
			printf("[%6.2f] rx stats (5 s): %d items, %d beacons, %d data, %d notifications\n",
				(double)system_time() / 1000000.0, items, beacons, datas, notifs);
			{
				struct iwi_status st;
				memset(&st, 0, sizeof(st));
				if (ioctl(gFd, IWI_IOC_STATUS, &st, sizeof(st)) >= 0)
					printf("          card: intr_seen %08lx intr %08lx rst %08lx ctl %08lx io %08lx cmd_ridx %lu rx_ridx %lu rx_cur %lu tx_ridx %lu tx_cur %lu rx_widx %lu mask %lx\n",
						st.v[0], st.v[1], st.v[2], st.v[3], st.v[4], st.v[5], st.v[6], st.v[7], st.v[8], st.v[9], st.v[10], st.v[11]);
				if (st.v[0] & 0x40000000) {
					int e, nn = st.v[13] > 4 ? 4 : (int)st.v[13];
					printf("          FIRMWARE ERROR LOG: base %08lx, %lu entries\n", st.v[12], st.v[13]);
					for (e = 0; e < nn; e++)
						printf("            #%d desc %lu time %lu blink %08lx %08lx link %08lx %08lx data %08lx\n", e,
							st.v[14 + e * 7], st.v[15 + e * 7], st.v[16 + e * 7], st.v[17 + e * 7],
							st.v[18 + e * 7], st.v[19 + e * 7], st.v[20 + e * 7]);
				}
			}
			fflush(stdout);
			items = beacons = datas = notifs = 0;
			last = system_time();
		}
	}
	if (gLogMgmt && n > 0) {
		if (buf[0] == 3 && buf[12] != 25)
			printf("[%6.2f] notification type %d flags %d len %d data %02x %02x %02x %02x\n",
				(double)system_time() / 1000000.0, buf[12], buf[13], buf[14] | (buf[15] << 8),
				buf[16], buf[17], buf[18], buf[19]);
		else if (buf[0] == 9 && n > 30 && ((buf[28] & 0xfc) == 0xc0 || (buf[28] & 0xfc) == 0xa0 || (buf[28] & 0xfc) == 0xb0
				|| (buf[28] & 0xfc) == 0x10 || (buf[28] & 0xfc) == 0x30))
			printf("[%6.2f] MGMT frame subtype 0x%02x len %d reason/status %d %d\n",
				(double)system_time() / 1000000.0, buf[28] & 0xfc, n - 28, buf[28 + 24], buf[28 + 25]);
		fflush(stdout);
	}
	return n;
}

static int
tx_frame(const u8* frame, int len)
{
	int tries, n;

	for (tries = 0; tries < 50; tries++) {
		n = write(gFd, frame, len);
		if (n == len)
			return 0;
		snooze(2000);
	}
	return -1;
}

/* ------------------------------ configuration file ------------------------------ */

#define MAX_NETS	8

typedef struct {
	char ssid[64];
	char psk[128];
	u8 pmk[32];
	int have_pmk;
} net_t;

static net_t gNets[MAX_NETS];
static int gNumNets = 0;
static char gSsid[64];			/* the network we are currently trying / connected to */
static char gPrefer[64];		/* a network the panel asked for: tried first */
static char gFwPath[256] = DEFAULT_FW, gGw[32] = "192.168.0.1";

/* Each "ssid=" line starts a new saved network, the "psk=" line after it belongs to it.  A file with no usable
   network is not an error: the daemon still runs, scans, and waits for the panel to add one. */
static int
read_config(const char* path)
{
	FILE* f = fopen(path, "r");
	char line[256];
	int i, k;

	gNumNets = 0;
	if (f == NULL) {
		say("no configuration file %s yet", path);
		return 0;
	}
	while (fgets(line, sizeof(line), f)) {
		int n = (int)strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';
		if (strncmp(line, "ssid=", 5) == 0) {
			if (gNumNets < MAX_NETS) {
				memset(&gNets[gNumNets], 0, sizeof(net_t));
				strncpy(gNets[gNumNets].ssid, line + 5, sizeof(gNets[0].ssid) - 1);
				gNumNets++;
			}
		} else if (strncmp(line, "psk=", 4) == 0) {
			if (gNumNets > 0)
				strncpy(gNets[gNumNets - 1].psk, line + 4, sizeof(gNets[0].psk) - 1);
		} else if (strncmp(line, "fw=", 3) == 0)
			strncpy(gFwPath, line + 3, sizeof(gFwPath) - 1);
		else if (strncmp(line, "gw=", 3) == 0)
			strncpy(gGw, line + 3, sizeof(gGw) - 1);
	}
	fclose(f);
	/* drop incomplete entries (no passphrase of 8..63 characters) */
	for (i = 0, k = 0; i < gNumNets; i++) {
		if (gNets[i].ssid[0] != '\0' && strlen(gNets[i].psk) >= 8 && strlen(gNets[i].psk) <= 63) {
			if (k != i)
				gNets[k] = gNets[i];
			k++;
		} else
			say("ignoring saved network '%s': it needs a password of 8 to 63 characters", gNets[i].ssid);
	}
	gNumNets = k;
	say("%d saved network%s", gNumNets, gNumNets == 1 ? "" : "s");
	return 0;
}

/* ------------------------------ card setup + scan ------------------------------ */

typedef struct {
	u8 bssid[6];
	int chan, rssi;
	unsigned int capinfo, intval;
	u8 tstamp[8];
	u8 rates[16];
	int nrates;
	u8 rsn[64];
	int rsnlen;
	int wpa1;
	char ssid[33];
} ap_t;

static ap_t gAp[MAX_AP];
static int gNumAp = 0;

static int
setup_card(void)
{
	static u8 buf[128];
	u32 d32;
	int i, r;
	static const u8 rates[12] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };

	r = fw_cmd(CMD_SET_MAC_ADDRESS, gMac, 6);
	if (r) return r;
	memset(buf, 0, 20);
	buf[4] = 1; buf[15] = 30; buf[18] = 1;
	if ((r = fw_cmd(CMD_SET_CONFIG, buf, 20))) return r;
	d32 = 0;
	if ((r = fw_cmd(CMD_SET_POWER_MODE, &d32, 4))) return r;
	d32 = 2346;
	if ((r = fw_cmd(CMD_SET_RTS_THRESHOLD, &d32, 4))) return r;
	if ((r = fw_cmd(CMD_SET_FRAG_THRESHOLD, &d32, 4))) return r;
	for (i = 0; i < 2; i++) {
		int c;
		memset(buf, 0, 76);
		buf[0] = 13;
		buf[1] = (i == 0) ? 2 : 1;
		for (c = 0; c < 13; c++) {
			buf[2 + c * 2] = c + 1;
			buf[3 + c * 2] = 20;
		}
		if ((r = fw_cmd(CMD_SET_TX_POWER, buf, 76))) return r;
	}
	memset(buf, 0, 16);
	buf[0] = 2; buf[1] = 12; buf[2] = 1;
	memcpy(buf + 4, rates, 12);
	if ((r = fw_cmd(CMD_SET_RATES, buf, 16))) return r;
	memset(buf, 0, 16);
	buf[2] = 1;
	if ((r = fw_cmd(CMD_SET_RATES, buf, 16))) return r;
	random_bytes((u8*)&d32, 4);
	if ((r = fw_cmd(CMD_SET_RANDOM_SEED, &d32, 4))) return r;
	return fw_cmd(CMD_ENABLE, NULL, 0);
}

static void
note_beacon(const u8* item, int n)
{
	const u8* wh = item + 28;
	int len = n - 28, fc = wh[0] & 0xfc, i, j, ssidlen = 0;
	const u8* ie;
	const u8* end;
	ap_t* a;

	if (len < 36 || (fc != 0x80 && fc != 0x50))
		return;
	for (j = 0; j < gNumAp; j++)
		if (memcmp(gAp[j].bssid, wh + 16, 6) == 0)
			break;
	if (j == gNumAp) {
		if (gNumAp == MAX_AP)
			return;
		gNumAp++;
		memset(&gAp[j], 0, sizeof(ap_t));
	}
	a = &gAp[j];
	memcpy(a->bssid, wh + 16, 6);
	a->chan = item[12];
	a->rssi = item[17];
	memcpy(a->tstamp, wh + 24, 8);
	a->intval = wh[24 + 8] | (wh[24 + 9] << 8);
	a->capinfo = wh[24 + 10] | (wh[24 + 11] << 8);
	ie = wh + 24 + 12;
	end = wh + len;
	a->nrates = 0;
	a->rsnlen = 0;
	while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
		if (ie[0] == 0) {
			ssidlen = ie[1] > 32 ? 32 : ie[1];
			for (i = 0; i < ssidlen; i++)
				a->ssid[i] = (ie[2 + i] < 32 || ie[2 + i] == 127 || ie[2 + i] == '|') ? '?' : ie[2 + i];
			a->ssid[ssidlen] = '\0';
		} else if (ie[0] == 3 && ie[1] >= 1) {
			a->chan = ie[2];
		} else if (ie[0] == 1 || ie[0] == 50) {
			for (i = 0; i < ie[1] && a->nrates < 16; i++)
				a->rates[a->nrates++] = ie[2 + i];
		} else if (ie[0] == 221 && ie[1] >= 4 && ie[2] == 0x00 && ie[3] == 0x50 && ie[4] == 0xf2 && ie[5] == 0x01) {
			a->wpa1 = 1;
		} else if (ie[0] == 48 && ie[1] <= 62) {
			memcpy(a->rsn, ie, 2 + ie[1]);
			a->rsnlen = 2 + ie[1];
		}
		ie += 2 + ie[1];
	}
}

static int
scan_for_aps(void)
{
	static u8 buf[128], item[2048];
	int i, n, done = 0;
	bigtime_t t0;

	gNumAp = 0;
	memset(buf, 0, 96);
	buf[4] = 0x40 | 13;
	for (i = 1; i <= 13; i++)
		buf[4 + i] = i;
	memset(buf + 4 + 54, 0x33, 27);
	buf[4 + 54 + 27 + 1 + 2 + 4] = 40;
	if (fw_cmd(CMD_SCAN, buf, 96) != 0)
		return -1;
	t0 = system_time();
	while (!done && system_time() - t0 < 6000000) {
		n = rx_item(item);
		if (n == 0) {
			snooze(2000);
			continue;
		}
		if (item[0] == ITEM_FRAME)
			note_beacon(item, n);
		else if (item[0] == ITEM_NOTIF && item[12] == NOTIF_SCAN_COMPLETE)
			done = 1;
	}
	return done ? gNumAp : -2;
}

/* ------------------------------ association ------------------------------ */

static const u8 kOurRsn[22] = {
	0x30, 0x14, 0x01, 0x00,
	0x00, 0x0f, 0xac, 0x04,			/* group cipher CCMP */
	0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,	/* 1 pairwise cipher: CCMP */
	0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,	/* 1 AKM: PSK */
	0x00, 0x00				/* RSN capabilities */
};

static int
associate(const ap_t* a)
{
	static u8 buf[128], item[2048];
	static const u8 ours[12] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
	u32 d32;
	int i, j, n, ssidlen = (int)strlen(gSsid), nr = 0, r;
	bigtime_t t0;
	int authed = 0, assoc = 0;

	/* configuration for a connection: the host decrypts */
	memset(buf, 0, 20);
	buf[4] = 1;	/* multicast_enabled */
	buf[6] = 1;	/* disable_unicast_decryption */
	buf[8] = 1;	/* disable_multicast_decryption */
	buf[11] = 1;	/* bg_autodetection */
	buf[15] = 30;	/* silence_threshold */
	buf[17] = 0;	/* allow_mgt OFF: with it on, the stream of other networks' beacons crashes the firmware (SYSASSERT) */
	buf[18] = 1;	/* report_noise */
	if ((r = fw_cmd(CMD_SET_CONFIG, buf, 20)) != 0) { say("set config failed %d", r); return -1; }
	if ((r = fw_cmd(CMD_SET_ESSID, gSsid, ssidlen)) != 0) { say("set essid failed %d", r); return -1; }

	/* negotiated rates: the AP's rates that we also support */
	memset(buf, 0, 16);
	buf[0] = 2;	/* 11g */
	buf[2] = 0;	/* NEGOTIATED */
	for (i = 0; i < a->nrates && nr < 12; i++) {
		for (j = 0; j < 12; j++) {
			if ((a->rates[i] & 0x7f) == ours[j]) {
				buf[4 + nr++] = a->rates[i];
				break;
			}
		}
	}
	buf[1] = nr;
	if ((r = fw_cmd(CMD_SET_RATES, buf, 16)) != 0) { say("set rates failed %d", r); return -1; }

	/* struct iwi_sensitivity { s16 rssi (dBm); u16 reserved }.  The frame byte is dBm + 112 (IWI_RSSI_TO_DBM). */
	{
		int dbm = a->rssi - 112;
		u8 sens[4];

		sens[0] = (u8)(dbm & 0xff);
		sens[1] = (u8)((dbm >> 8) & 0xff);
		sens[2] = sens[3] = 0;
		say("sensitivity %d dBm", dbm);
		if ((r = fw_cmd(CMD_SET_SENSITIVITY, sens, 4)) != 0) { say("set sensitivity failed %d", r); return -1; }
	}
	if ((r = fw_cmd(CMD_SET_OPTIE, kOurRsn, sizeof(kOurRsn))) != 0) { say("set optie failed %d", r); return -1; }

	/* struct iwi_associate (40 bytes) */
	memset(buf, 0, 40);
	buf[0] = a->chan;
	buf[1] = 0;				/* auth: open system */
	buf[2] = 0;				/* IWI_ASSOC_ASSOCIATE */
	buf[4] = 2; buf[5] = 0;			/* policy: RSN */
	buf[6] = 0;				/* plen */
	buf[7] = 2;				/* mode: 11g */
	memcpy(buf + 8, a->bssid, 6);
	memcpy(buf + 14, a->tstamp, 8);
	buf[22] = 0x01; buf[23] = 0x00;		/* capinfo: ESS */
	buf[24] = 10; buf[25] = 0;		/* listen interval */
	buf[26] = a->intval & 0xff; buf[27] = a->intval >> 8;
	memcpy(buf + 28, a->bssid, 6);		/* dst */
	if ((r = fw_cmd(CMD_ASSOCIATE, buf, 40)) != 0) { say("associate command failed %d", r); return -1; }

	t0 = system_time();
	while (system_time() - t0 < 8000000) {
		n = rx_item(item);
		if (n == 0) {
			snooze(2000);
			continue;
		}
		if (item[0] == ITEM_NOTIF && item[12] == NOTIF_AUTHENTICATION) {
			say("authentication notification: state %d", item[16]);
			if (item[16] == 9)
				authed = 1;
		} else if (item[0] == ITEM_NOTIF && item[12] == NOTIF_ASSOCIATION) {
			say("association notification: state %d status %d", item[16], item[43] | (item[44] << 8));
			if (item[16] == 12) {
				assoc = 1;
				break;
			}
		}
	}
	if (!assoc)
		say("not associated (authenticated=%d)", authed);
	return assoc ? 0 : -1;
}

/* ------------------------------ EAPOL 4-way handshake ------------------------------ */

static u8 gPmk[32];
static u8 gPtk[48];		/* KCK(16) KEK(16) TK(16) */
static u8 gGtk[32];
static int gGtkIdx = 1, gHaveGtk = 0;
static u8 gBssid[6];
static aes_ctx gTkCtx, gGtkCtx;
static u8 gTxPn[6];		/* 48-bit packet number, little-endian */
static u8 gRxPnPtk[6];

/* build an 802.11 data frame (To-DS) carrying an LLC/SNAP payload.  hdr 24 bytes, payload follows. */
static int
build_data_to_ds(u8* out, const u8* da, int protect, const u8* payload, int plen)
{
	u8* p = out;
	unsigned int sc;

	p[0] = 0x08;
	p[1] = 0x01 | (protect ? 0x40 : 0);
	p[2] = p[3] = 0;
	memcpy(p + 4, gBssid, 6);
	memcpy(p + 10, gMac, 6);
	memcpy(p + 16, da, 6);
	sc = (gSeq++ & 0xfff) << 4;
	p[22] = sc & 0xff;
	p[23] = sc >> 8;
	if (!protect) {
		memcpy(p + 24, payload, plen);
		return 24 + plen;
	}
	/* CCMP: header + ciphertext + MIC */
	{
		u8 nonce[13], aad[22], mic[8];
		int i;

		/* PN increment */
		for (i = 0; i < 6; i++) {
			if (++gTxPn[i] != 0)
				break;
		}
		p[24] = gTxPn[0]; p[25] = gTxPn[1]; p[26] = 0; p[27] = 0x20;
		p[28] = gTxPn[2]; p[29] = gTxPn[3]; p[30] = gTxPn[4]; p[31] = gTxPn[5];
		nonce[0] = 0;
		memcpy(nonce + 1, gMac, 6);
		for (i = 0; i < 6; i++)
			nonce[7 + i] = gTxPn[5 - i];
		aad[0] = p[0] & 0x8f;
		aad[1] = p[1] & 0xc7;
		memcpy(aad + 2, p + 4, 18);
		aad[20] = p[22] & 0x0f;
		aad[21] = 0;
		memcpy(p + 32, payload, plen);
		ccm_encrypt(&gTkCtx, nonce, aad, 22, p + 32, plen, mic);
		memcpy(p + 32 + plen, mic, 8);
		return 32 + plen + 8;
	}
}

static const u8 kSnapEapol[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e };

static int
send_eapol(const u8* eapol, int len)
{
	static u8 payload[512], frame[600];
	int n;

	memcpy(payload, kSnapEapol, 8);
	memcpy(payload + 8, eapol, len);
	n = build_data_to_ds(frame, gBssid, 0, payload, 8 + len);
	return tx_frame(frame, n);
}

/* wait for the next EAPOL-Key frame from the AP; returns its length or 0 on timeout.  eapol points into item */
static int
wait_eapol(u8* item, const u8** eapol, int timeout_ms)
{
	bigtime_t t0 = system_time();
	int n;

	while (system_time() - t0 < (bigtime_t)timeout_ms * 1000) {
		n = rx_item(item);
		if (n == 0) {
			snooze(2000);
			continue;
		}
		if (item[0] == ITEM_FRAME && n >= 28 + 24 + 8 + 4) {
			const u8* wh = item + 28;
			if ((wh[0] & 0x0c) == 0x08 && !(wh[1] & 0x40) && memcmp(wh + 24, kSnapEapol, 8) == 0
					&& memcmp(wh + 10, gBssid, 6) == 0) {
				*eapol = wh + 24 + 8;
				return n - 28 - 24 - 8;
			}
		} else if (item[0] == ITEM_NOTIF && item[12] == NOTIF_ASSOCIATION) {
			say("association notification during handshake: state %d", item[16]);
		}
	}
	return 0;
}

static void
put_be16(u8* p, unsigned int v)
{
	p[0] = v >> 8;
	p[1] = v & 0xff;
}

static unsigned int
get_be16(const u8* p)
{
	return (p[0] << 8) | p[1];
}

/* EAPOL-Key frame layout (offsets from the start of the EAPOL header) */
#define K_DESC		4
#define K_INFO		5
#define K_LEN		7
#define K_REPLAY	9
#define K_NONCE		17
#define K_IV		49
#define K_RSC		65
#define K_ID		73
#define K_MIC		81
#define K_DLEN		97
#define K_DATA		99

static void
eapol_mic(const u8* kck, u8* frame, int len)
{
	u8 mic[20];

	memset(frame + K_MIC, 0, 16);
	hmac_sha1(kck, 16, frame, len, mic);
	memcpy(frame + K_MIC, mic, 16);
}

static int
do_handshake(void)
{
	static u8 item[2048], f2[256], f4[128];
	const u8* m;
	u8 anonce[32], snonce[32], data[2 * 6 + 2 * 32], replay[8];
	u8 mic_save[16], mic_calc[20];
	int n, len2, tries, info, i, got1 = 0;
	static u8 plain[256];

	random_bytes(snonce, 32);
	say("waiting for EAPOL message 1 from the AP");
	for (tries = 0; tries < 6 && !got1; tries++) {
		n = wait_eapol(item, &m, 2000);
		if (n == 0)
			continue;
		if (m[1] != 3 || m[K_DESC] != 2)
			continue;
		info = get_be16(m + K_INFO);
		if ((info & 0x0188) == 0x0088) {	/* ack + pairwise, no MIC: message 1 */
			got1 = 1;
			memcpy(anonce, m + K_NONCE, 32);
			memcpy(replay, m + K_REPLAY, 8);
			if ((info & 7) != 2)
				say("note: key descriptor version %d (expected 2)", info & 7);
		}
	}
	if (!got1) {
		say("no EAPOL message 1 received");
		return -1;
	}
	say("message 1 received (key info 0x%04x)", info);

	/* PTK = PRF-384(PMK, "Pairwise key expansion", min(AA,SPA) || max(AA,SPA) || min(AN,SN) || max(AN,SN)) */
	if (memcmp(gBssid, gMac, 6) < 0) {
		memcpy(data, gBssid, 6);
		memcpy(data + 6, gMac, 6);
	} else {
		memcpy(data, gMac, 6);
		memcpy(data + 6, gBssid, 6);
	}
	if (memcmp(anonce, snonce, 32) < 0) {
		memcpy(data + 12, anonce, 32);
		memcpy(data + 44, snonce, 32);
	} else {
		memcpy(data + 12, snonce, 32);
		memcpy(data + 44, anonce, 32);
	}
	wpa_prf(gPmk, 32, "Pairwise key expansion", data, sizeof(data), gPtk, 48);

	/* message 2: our SNonce + RSN IE, MIC'd with the KCK */
	memset(f2, 0, sizeof(f2));
	f2[0] = 2;				/* EAPOL version */
	f2[1] = 3;				/* EAPOL-Key */
	f2[K_DESC] = 2;				/* RSN key descriptor */
	put_be16(f2 + K_INFO, 0x010a);		/* ver 2 | pairwise | MIC */
	put_be16(f2 + K_LEN, 0);
	memcpy(f2 + K_REPLAY, replay, 8);
	memcpy(f2 + K_NONCE, snonce, 32);
	put_be16(f2 + K_DLEN, sizeof(kOurRsn));
	memcpy(f2 + K_DATA, kOurRsn, sizeof(kOurRsn));
	len2 = K_DATA + sizeof(kOurRsn);
	put_be16(f2 + 2, len2 - 4);
	eapol_mic(gPtk, f2, len2);
	if (send_eapol(f2, len2) != 0) {
		say("could not transmit message 2");
		return -1;
	}
	say("message 2 sent");

	/* message 3: verify MIC, unwrap the GTK */
	for (tries = 0; tries < 8; tries++) {
		int dlen, ok = 0;
		n = wait_eapol(item, &m, 2000);
		if (n == 0) {
			say("timeout waiting for message 3 (the AP may have rejected message 2: wrong password?)");
			continue;
		}
		if (m[1] != 3 || m[K_DESC] != 2)
			continue;
		info = get_be16(m + K_INFO);
		if ((info & 0x0188) == 0x0088 && !(info & 0x100)) {
			/* a repeated message 1: answer again with the same message 2 */
			send_eapol(f2, len2);
			continue;
		}
		if ((info & 0x01c8) != 0x01c8 && (info & 0x0148) != 0x0148)
			continue;
		/* verify MIC over a copy with the MIC zeroed */
		{
			static u8 copy[512];
			int elen = 4 + get_be16(m + 2);
			if (elen > (int)sizeof(copy) || elen > n)
				continue;
			memcpy(copy, m, elen);
			memcpy(mic_save, copy + K_MIC, 16);
			memset(copy + K_MIC, 0, 16);
			hmac_sha1(gPtk, 16, copy, elen, mic_calc);
			ok = memcmp(mic_save, mic_calc, 16) == 0;
		}
		if (!ok) {
			say("message 3 MIC does not verify (wrong passphrase?)");
			return -2;
		}
		say("message 3 received, MIC verified (key info 0x%04x)", info);
		memcpy(replay, m + K_REPLAY, 8);
		dlen = get_be16(m + K_DLEN);
		if (dlen > 0 && (info & 0x1000)) {
			if (dlen < 24 || dlen > (int)sizeof(plain) + 8 || aes_unwrap(gPtk + 16, m + K_DATA, dlen, plain) != 0) {
				say("key data unwrap failed");
				return -3;
			}
			dlen -= 8;
		} else {
			memcpy(plain, m + K_DATA, dlen);
		}
		/* walk the key data: look for the GTK KDE (dd len 00 0f ac 01 keyid rsvd gtk) */
		gHaveGtk = 0;
		for (i = 0; i + 2 <= dlen; ) {
			int l = plain[i + 1];
			if (plain[i] == 0xdd && l >= 22 && plain[i + 2] == 0x00 && plain[i + 3] == 0x0f
					&& plain[i + 4] == 0xac && plain[i + 5] == 0x01) {
				int gl = l - 6;
				gGtkIdx = plain[i + 6] & 3;
				if (gl > 32)
					gl = 32;
				memcpy(gGtk, plain + i + 8, gl);
				if (gl == 16)
					gHaveGtk = 1;
				else
					say("group key is %d bytes (TKIP?), not CCMP: group traffic unsupported", gl);
			}
			if (plain[i] == 0xdd && l == 0)
				break;
			i += 2 + l;
		}
		/* message 4 */
		memset(f4, 0, sizeof(f4));
		f4[0] = 2; f4[1] = 3; f4[K_DESC] = 2;
		put_be16(f4 + K_INFO, 0x030a);	/* ver 2 | pairwise | MIC | secure */
		memcpy(f4 + K_REPLAY, replay, 8);
		put_be16(f4 + K_DLEN, 0);
		put_be16(f4 + 2, K_DATA - 4);
		eapol_mic(gPtk, f4, K_DATA);
		if (send_eapol(f4, K_DATA) != 0) {
			say("could not transmit message 4");
			return -1;
		}
		say("message 4 sent: handshake complete%s", gHaveGtk ? ", GTK installed" : " (no usable GTK)");
		aes_set_key(&gTkCtx, gPtk + 32);
		if (gHaveGtk)
			aes_set_key(&gGtkCtx, gGtk);
		memset(gTxPn, 0, 6);
		memset(gRxPnPtk, 0, 6);
		return 0;
	}
	return -1;
}


/* EAPOL after the pairwise key is installed travels encrypted */
static int
send_eapol_enc(const u8* eapol, int len)
{
	static u8 payload[512], frame[640];
	int n;

	memcpy(payload, kSnapEapol, 8);
	memcpy(payload + 8, eapol, len);
	n = build_data_to_ds(frame, gBssid, 1, payload, 8 + len);
	return tx_frame(frame, n);
}

/* the AP refreshes the group key from time to time (EAPOL group key handshake) */
static void
group_rekey(const u8* m, int n)
{
	static u8 plain[256], copy[512], f2[128];
	u8 mic_calc[20], replay[8];
	int info, dlen, elen, i;

	if (n < K_DATA || m[1] != 3 || m[K_DESC] != 2)
		return;
	info = get_be16(m + K_INFO);
	if (info & 0x08) {
		say("EAPOL pairwise message after the handshake: key info 0x%04x (retransmission?)", info);
		return;				/* a pairwise message (PTK rekey): not supported */
	}
	elen = 4 + get_be16(m + 2);
	if (elen > (int)sizeof(copy) || elen > n)
		return;
	memcpy(copy, m, elen);
	memset(copy + K_MIC, 0, 16);
	hmac_sha1(gPtk, 16, copy, elen, mic_calc);
	if (memcmp(m + K_MIC, mic_calc, 16) != 0) {
		say("group key message: bad MIC, ignored");
		return;
	}
	memcpy(replay, m + K_REPLAY, 8);
	dlen = get_be16(m + K_DLEN);
	if (dlen > 0 && (info & 0x1000)) {
		if (dlen < 24 || dlen > (int)sizeof(plain) + 8 || aes_unwrap(gPtk + 16, m + K_DATA, dlen, plain) != 0) {
			say("group key unwrap failed");
			return;
		}
		dlen -= 8;
	} else {
		return;
	}
	for (i = 0; i + 2 <= dlen; ) {
		int l = plain[i + 1];
		if (plain[i] == 0xdd && l >= 22 && plain[i + 2] == 0x00 && plain[i + 3] == 0x0f
				&& plain[i + 4] == 0xac && plain[i + 5] == 0x01 && l - 6 == 16) {
			gGtkIdx = plain[i + 6] & 3;
			memcpy(gGtk, plain + i + 8, 16);
			aes_set_key(&gGtkCtx, gGtk);
			gHaveGtk = 1;
		}
		if (plain[i] == 0xdd && l == 0)
			break;
		i += 2 + l;
	}
	memset(f2, 0, sizeof(f2));
	f2[0] = 2; f2[1] = 3; f2[K_DESC] = 2;
	put_be16(f2 + K_INFO, 0x0302);		/* ver 2 | MIC | secure: group key message 2 */
	memcpy(f2 + K_REPLAY, replay, 8);
	put_be16(f2 + 2, K_DATA - 4);
	eapol_mic(gPtk, f2, K_DATA);
	send_eapol_enc(f2, K_DATA);
	say("group key refreshed");
}

/* ------------------------------ data path: decrypt a received frame to an Ethernet frame ------------------------------ */

/* returns Ethernet frame length (DA SA type payload) in out, or -1 */
static int
rx_to_ethernet(const u8* item, int n, u8* out)
{
	const u8* wh = item + 28;
	int len = n - 28, hdrlen = 24, plen, i, keyid, tid = 0, bcast;
	u8 nonce[13], aad[24], mic[8], pn[6];
	static u8 body[2048];
	const aes_ctx* ctx;
	int aadlen;

	if (len < 24 + 8 + 8 || (wh[0] & 0x0c) != 0x08)
		return -1;
	if ((wh[0] & 0x80)) {			/* QoS data */
		hdrlen = 26;
		tid = wh[24] & 0x0f;
	}
	if ((wh[1] & 0x03) != 0x02)		/* From-DS only */
		return -1;
	if (!(wh[1] & 0x40))
		return -1;			/* unencrypted data: ignore (EAPOL is handled separately) */
	if (len < hdrlen + 8 + 8)
		return -1;
	bcast = wh[4] & 1;
	keyid = wh[hdrlen + 3] >> 6;
	if (!(wh[hdrlen + 3] & 0x20))
		return -1;			/* no ExtIV: not CCMP */
	ctx = bcast ? &gGtkCtx : &gTkCtx;
	if (bcast && !gHaveGtk)
		return -1;
	pn[0] = wh[hdrlen + 7]; pn[1] = wh[hdrlen + 6]; pn[2] = wh[hdrlen + 5];
	pn[3] = wh[hdrlen + 4]; pn[4] = wh[hdrlen + 1]; pn[5] = wh[hdrlen + 0];
	nonce[0] = tid;
	memcpy(nonce + 1, wh + 10, 6);
	memcpy(nonce + 7, pn, 6);
	aad[0] = wh[0] & 0x8f;
	aad[1] = wh[1] & 0xc7;
	memcpy(aad + 2, wh + 4, 18);
	aad[20] = wh[22] & 0x0f;
	aad[21] = 0;
	aadlen = 22;
	if (hdrlen == 26) {
		aad[22] = tid;
		aad[23] = 0;
		aadlen = 24;
	}
	plen = len - hdrlen - 8 - 8;
	memcpy(mic, wh + len - 8, 8);
	memcpy(body, wh + hdrlen + 8, plen);
	(void)keyid;
	if (ccm_decrypt(ctx, nonce, aad, aadlen, body, plen, mic) != 0) {
		if (gVerbose)
			say("CCMP MIC failure on a %d byte frame (%s)", len, bcast ? "group" : "unicast");
		return -1;
	}
	if (plen < 8 || body[0] != 0xaa || body[1] != 0xaa || body[2] != 0x03)
		return -1;
	memcpy(out, wh + 4, 6);			/* DA */
	memcpy(out + 6, wh + 16, 6);		/* SA */
	memcpy(out + 12, body + 6, plen - 6);	/* ethertype + data */
	(void)i;
	return 12 + plen - 6;
}

/* send an Ethernet frame (DA SA type data) over the encrypted link */
static int
send_ethernet(const u8* eth, int len)
{
	static u8 payload[1600], frame[1700];
	int n;

	if (len < 14 || len > 1514)
		return -1;
	memcpy(payload, kSnapEapol, 6);		/* aa aa 03 00 00 00 */
	memcpy(payload + 6, eth + 12, len - 12);
	n = build_data_to_ds(frame, eth, 1, payload, 6 + len - 12);
	/* DA in addr3 is eth[0..5]; SA is our MAC (addr2) */
	return tx_frame(frame, n);
}



/* ------------------------------ DHCP client (runs over the encrypted link, independent of the BONE stack) ------------------------------ */

static u8 gIp[4], gMask[4], gRouter[4], gDns[4], gServer[4];
static u32 gLease = 0;
static bigtime_t gLeaseStart = 0;
static int gTakeOver = 0;	/* -r: also install the default route and DNS */

static unsigned int
ip_checksum(const u8* p, int n)
{
	unsigned long sum = 0;
	int i;

	for (i = 0; i + 1 < n; i += 2)
		sum += (p[i] << 8) | p[i + 1];
	if (n & 1)
		sum += p[n - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (~sum) & 0xffff;
}

/* msgtype 1 = DISCOVER, 3 = REQUEST */
static int
dhcp_send(int msgtype, u32 xid, const u8* req_ip, const u8* server_id, const u8* ciaddr)
{
	static u8 f[600];
	u8* ip = f + 14;
	u8* udp = ip + 20;
	u8* b = udp + 8;
	u8* o;
	int blen, ulen, total;

	memset(f, 0, sizeof(f));
	memset(f, 0xff, 6);
	memcpy(f + 6, gMac, 6);
	f[12] = 0x08; f[13] = 0x00;

	b[0] = 1; b[1] = 1; b[2] = 6; b[3] = 0;
	b[4] = xid >> 24; b[5] = xid >> 16; b[6] = xid >> 8; b[7] = xid;
	b[10] = 0x80;				/* flags: broadcast */
	if (ciaddr)
		memcpy(b + 12, ciaddr, 4);
	memcpy(b + 28, gMac, 6);
	b[236] = 0x63; b[237] = 0x82; b[238] = 0x53; b[239] = 0x63;
	o = b + 240;
	*o++ = 53; *o++ = 1; *o++ = msgtype;
	*o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, gMac, 6); o += 6;
	if (req_ip) { *o++ = 50; *o++ = 4; memcpy(o, req_ip, 4); o += 4; }
	if (server_id) { *o++ = 54; *o++ = 4; memcpy(o, server_id, 4); o += 4; }
	*o++ = 12; *o++ = 4; memcpy(o, "zeta", 4); o += 4;
	*o++ = 55; *o++ = 5; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15; *o++ = 51;
	*o++ = 255;
	blen = (int)(o - b);
	if (blen < 300)
		blen = 300;
	ulen = 8 + blen;
	udp[0] = 0; udp[1] = 68; udp[2] = 0; udp[3] = 67;
	udp[4] = ulen >> 8; udp[5] = ulen & 0xff;
	total = 20 + ulen;
	ip[0] = 0x45; ip[2] = total >> 8; ip[3] = total & 0xff;
	ip[4] = 0; ip[5] = (u8)(xid & 0xff);
	ip[8] = 64; ip[9] = 17;
	memset(ip + 12, 0, 4);
	memset(ip + 16, 0xff, 4);
	{
		unsigned int ck = ip_checksum(ip, 20);
		ip[10] = ck >> 8; ip[11] = ck & 0xff;
	}
	return send_ethernet(f, 14 + total);
}

/* look at a decrypted Ethernet frame: if it is a DHCP reply for xid, fill type + fields. returns type or 0 */
static int
dhcp_parse(const u8* eth, int len, u32 xid, u8* yiaddr, u8* mask, u8* router, u8* dns, u8* server, u32* lease)
{
	const u8* ip;
	const u8* udp;
	const u8* b;
	const u8* o;
	const u8* end;
	int ihl, type = 0;

	if (len < 14 + 20 + 8 + 240 || eth[12] != 0x08 || eth[13] != 0x00)
		return 0;
	ip = eth + 14;
	ihl = (ip[0] & 0x0f) * 4;
	if ((ip[0] >> 4) != 4 || ip[9] != 17 || len < 14 + ihl + 8 + 240)
		return 0;
	udp = ip + ihl;
	if (udp[2] != 0 || udp[3] != 68)
		return 0;
	b = udp + 8;
	if (b[0] != 2 || b[236] != 0x63 || b[237] != 0x82 || b[238] != 0x53 || b[239] != 0x63)
		return 0;
	if (b[4] != (u8)(xid >> 24) || b[5] != (u8)(xid >> 16) || b[6] != (u8)(xid >> 8) || b[7] != (u8)xid)
		return 0;
	memcpy(yiaddr, b + 16, 4);
	o = b + 240;
	end = eth + len;
	while (o + 2 <= end && *o != 255) {
		if (*o == 0) { o++; continue; }
		if (o + 2 + o[1] > end)
			break;
		switch (*o) {
			case 53: type = o[2]; break;
			case 1: if (o[1] >= 4) memcpy(mask, o + 2, 4); break;
			case 3: if (o[1] >= 4) memcpy(router, o + 2, 4); break;
			case 6: if (o[1] >= 4) memcpy(dns, o + 2, 4); break;
			case 54: if (o[1] >= 4) memcpy(server, o + 2, 4); break;
			case 51: if (o[1] >= 4) *lease = ((u32)o[2] << 24) | (o[3] << 16) | (o[4] << 8) | o[5]; break;
		}
		o += 2 + o[1];
	}
	return type;
}

/* wait for a DHCP reply of the wanted type; returns 1 if found */
static int
dhcp_wait(int want, u32 xid, int ms, u8* yi, u8* mask, u8* router, u8* dns, u8* server, u32* lease)
{
	static u8 item[2048], eth[2048];
	bigtime_t t0 = system_time();
	int n, m, t;

	while (system_time() - t0 < (bigtime_t)ms * 1000) {
		n = rx_item(item);
		if (n == 0) {
			snooze(2000);
			continue;
		}
		if (item[0] != ITEM_FRAME)
			continue;
		m = rx_to_ethernet(item, n, eth);
		if (m < 0)
			continue;
		t = dhcp_parse(eth, m, xid, yi, mask, router, dns, server, lease);
		if (t == want)
			return 1;
		if (t == 6)
			return -1;		/* NAK */
	}
	return 0;
}

static int
dhcp_acquire(void)
{
	u8 yi[4], mask[4], router[4], dns[4], server[4];
	u32 lease = 0, xid;
	int attempt, r;
	char ipstr[20];

	for (attempt = 0; attempt < 4; attempt++) {
		random_bytes((u8*)&xid, 4);
		memset(mask, 0, 4); memset(router, 0, 4); memset(dns, 0, 4); memset(server, 0, 4);
		say("DHCP: discover (try %d)", attempt + 1);
		if (dhcp_send(1, xid, NULL, NULL, NULL) != 0)
			continue;
		if (dhcp_wait(2, xid, 4000, yi, mask, router, dns, server, &lease) != 1)
			continue;
		sprintf(ipstr, "%d.%d.%d.%d", yi[0], yi[1], yi[2], yi[3]);
		say("DHCP: offer %s from %d.%d.%d.%d", ipstr, server[0], server[1], server[2], server[3]);
		dhcp_send(3, xid, yi, server, NULL);
		r = dhcp_wait(5, xid, 4000, yi, mask, router, dns, server, &lease);
		if (r != 1)
			continue;
		memcpy(gIp, yi, 4); memcpy(gMask, mask, 4); memcpy(gRouter, router, 4);
		memcpy(gDns, dns, 4); memcpy(gServer, server, 4);
		gLease = lease ? lease : 3600;
		gLeaseStart = system_time();
		say("DHCP: lease %d.%d.%d.%d mask %d.%d.%d.%d router %d.%d.%d.%d dns %d.%d.%d.%d for %lu s",
			gIp[0], gIp[1], gIp[2], gIp[3], gMask[0], gMask[1], gMask[2], gMask[3],
			gRouter[0], gRouter[1], gRouter[2], gRouter[3], gDns[0], gDns[1], gDns[2], gDns[3], (unsigned long)gLease);
		return 0;
	}
	say("DHCP: no lease obtained");
	return -1;
}

/* renew: broadcast a REQUEST for the address we hold (many servers accept this) */
static void
dhcp_renew(void)
{
	u8 yi[4], mask[4], router[4], dns[4], server[4];
	u32 lease = 0, xid;

	random_bytes((u8*)&xid, 4);
	memcpy(mask, gMask, 4); memcpy(router, gRouter, 4); memcpy(dns, gDns, 4); memcpy(server, gServer, 4);
	dhcp_send(3, xid, gIp, gServer, gIp);
	if (dhcp_wait(5, xid, 4000, yi, mask, router, dns, server, &lease) == 1) {
		gLease = lease ? lease : gLease;
		gLeaseStart = system_time();
		say("DHCP: lease renewed for %lu s", (unsigned long)gLease);
	} else {
		say("DHCP: renew got no answer yet");
		gLeaseStart -= (bigtime_t)(gLease / 8) * 1000000;	/* try again a bit later */
	}
}

static void
apply_address(void)
{
	char cmd[200];

	sprintf(cmd, "ifconfig /dev/net/iwi/0 %d.%d.%d.%d %d.%d.%d.%d up", gIp[0], gIp[1], gIp[2], gIp[3],
		gMask[0], gMask[1], gMask[2], gMask[3]);
	say("running: %s", cmd);
	system(cmd);
	if (gTakeOver && gRouter[0]) {
		/* the boot-time default route (from /etc/network.conf) is still in place: remove it first */
		say("running: route delete default");
		system("route delete default > /dev/null 2>&1");
		sprintf(cmd, "route add default gw %d.%d.%d.%d dev /dev/net/iwi/0", gRouter[0], gRouter[1], gRouter[2], gRouter[3]);
		say("running: %s", cmd);
		system(cmd);
		if (gDns[0]) {
			FILE* f = fopen("/etc/resolv.conf", "w");
			if (f) {
				fprintf(f, "nameserver %d.%d.%d.%d\n", gDns[0], gDns[1], gDns[2], gDns[3]);
				fclose(f);
				say("DNS server set to %d.%d.%d.%d", gDns[0], gDns[1], gDns[2], gDns[3]);
			}
		}
	}
}


/* ------------------------------ debugging aids: answer / send ICMP echo ourselves (-e / -t) ------------------------------ */

static int gEchoResponder = 0, gSelfPing = 0;

/* if eth is an ICMP echo request for our address, answer it directly; returns 1 if handled */
static int
icmp_answer(const u8* eth, int len)
{
	static u8 r[1600];
	const u8* ip;
	int ihl, ilen;
	unsigned int ck;

	if (len < 14 + 20 + 8 || eth[12] != 0x08 || eth[13] != 0x00 || len > (int)sizeof(r))
		return 0;
	ip = eth + 14;
	ihl = (ip[0] & 0x0f) * 4;
	ilen = (ip[2] << 8) | ip[3];
	if ((ip[0] >> 4) != 4 || ip[9] != 1 || memcmp(ip + 16, gIp, 4) != 0 || ip[ihl] != 8 || 14 + ilen > len)
		return 0;
	memcpy(r, eth, len);
	memcpy(r, eth + 6, 6);			/* DA = their SA */
	memcpy(r + 6, gMac, 6);
	memcpy(r + 14 + 12, ip + 16, 4);	/* src = our IP */
	memcpy(r + 14 + 16, ip + 12, 4);	/* dst = their IP */
	r[14 + ihl] = 0;			/* echo reply */
	r[14 + ihl + 2] = r[14 + ihl + 3] = 0;
	ck = ip_checksum(r + 14 + ihl, ilen - ihl);
	r[14 + ihl + 2] = ck >> 8;
	r[14 + ihl + 3] = ck & 0xff;
	say("answering ICMP echo from %d.%d.%d.%d directly", ip[12], ip[13], ip[14], ip[15]);
	send_ethernet(r, 14 + ilen);
	return 1;
}

/* resolve an IPv4 address with ARP, using our own packets */
static int
arp_lookup(const u8* target, u8* mac_out)
{
	static u8 eth[64], item[2048], out[2048];
	int i, n;
	bigtime_t t0;

	memset(eth, 0xff, 6);
	memcpy(eth + 6, gMac, 6);
	eth[12] = 0x08; eth[13] = 0x06;
	eth[14] = 0; eth[15] = 1; eth[16] = 0x08; eth[17] = 0; eth[18] = 6; eth[19] = 4; eth[20] = 0; eth[21] = 1;
	memcpy(eth + 22, gMac, 6);
	memcpy(eth + 28, gIp, 4);
	memset(eth + 32, 0, 6);
	memcpy(eth + 38, target, 4);
	for (i = 0; i < 3; i++) {
		send_ethernet(eth, 42);
		t0 = system_time();
		while (system_time() - t0 < 1000000) {
			n = rx_item(item);
			if (n == 0) { snooze(2000); continue; }
			if (item[0] != ITEM_FRAME)
				continue;
			n = rx_to_ethernet(item, n, out);
			if (n >= 42 && out[12] == 0x08 && out[13] == 0x06 && out[21] == 2 && memcmp(out + 28, target, 4) == 0) {
				memcpy(mac_out, out + 22, 6);
				return 0;
			}
		}
	}
	return -1;
}

static void
self_ping(const u8* target)
{
	static u8 f[128], item[2048], out[2048];
	u8 tmac[6];
	char m[20];
	int k, n, got = 0;
	bigtime_t t0;
	unsigned int ck;

	if (arp_lookup(target, tmac) != 0) {
		say("self ping: no ARP answer from %d.%d.%d.%d", target[0], target[1], target[2], target[3]);
		return;
	}
	mac_str(tmac, m);
	say("self ping: %d.%d.%d.%d is at %s", target[0], target[1], target[2], target[3], m);
	for (k = 0; k < 4; k++) {
		memset(f, 0, sizeof(f));
		memcpy(f, tmac, 6); memcpy(f + 6, gMac, 6); f[12] = 0x08; f[13] = 0;
		f[14] = 0x45; f[16] = 0; f[17] = 28 + 32; f[18] = 0x12; f[19] = (u8)k; f[22] = 64; f[23] = 1;
		memcpy(f + 26, gIp, 4); memcpy(f + 30, target, 4);
		ck = ip_checksum(f + 14, 20);
		f[24] = ck >> 8; f[25] = ck & 0xff;
		f[34] = 8; f[35] = 0; f[38] = 0x42; f[39] = 0x42; f[40] = 0; f[41] = (u8)k;
		for (n = 0; n < 32; n++)
			f[42 + n] = n;
		ck = ip_checksum(f + 34, 8 + 32);
		f[36] = ck >> 8; f[37] = ck & 0xff;
		send_ethernet(f, 14 + 20 + 8 + 32);
		t0 = system_time();
		while (system_time() - t0 < 1500000) {
			n = rx_item(item);
			if (n == 0) { snooze(2000); continue; }
			if (item[0] != ITEM_FRAME)
				continue;
			n = rx_to_ethernet(item, n, out);
			if (n > 0 && out[12] == 0x08 && out[13] == 0 && out[14 + 9] == 1 && out[14 + 20] == 0) {
				say("self ping: ECHO REPLY %d from the router", k);
				got++;
				break;
			}
		}
	}
	say("self ping done: %d of 4 answered", got);
}


/* ------------------------------ panel interface: status file, scan file, command file ------------------------------ */

#define STATUS_FILE	"/tmp/iwid.status"
#define SCAN_FILE	"/tmp/iwid.scan"
#define CMD_FILE	"/tmp/iwid.cmd"

#define PCMD_NONE	0
#define PCMD_SCAN	1
#define PCMD_RELOAD	2

static int gCurDbm = -100;		/* smoothed signal of the access point we are talking to (dBm) */
static int gCurChan = 0;
static int gLinked = 0;			/* 1 once we are associated, handshaken and (normally) have an address */

/* The panel (or anyone) reads these small text files; they are written to a temp file and renamed, so a reader
   never sees a half-written one. */
static void
write_status(const char* state, const char* error)
{
	FILE* f = fopen(STATUS_FILE ".tmp", "w");
	char bssid[20];

	if (f == NULL)
		return;
	mac_str(gBssid, bssid);
	fprintf(f, "state=%s\n", state);
	fprintf(f, "error=%s\n", error ? error : "");
	fprintf(f, "ssid=%s\n", gLinked ? gSsid : "");
	fprintf(f, "bssid=%s\n", gLinked ? bssid : "");
	fprintf(f, "chan=%d\n", gLinked ? gCurChan : 0);
	fprintf(f, "dbm=%d\n", gLinked ? gCurDbm : 0);
	if (gLinked && gLease) {
		fprintf(f, "ip=%d.%d.%d.%d\n", gIp[0], gIp[1], gIp[2], gIp[3]);
		fprintf(f, "router=%d.%d.%d.%d\n", gRouter[0], gRouter[1], gRouter[2], gRouter[3]);
		fprintf(f, "dns=%d.%d.%d.%d\n", gDns[0], gDns[1], gDns[2], gDns[3]);
	} else {
		fprintf(f, "ip=\nrouter=\ndns=\n");
	}
	fprintf(f, "networks=%d\n", gNumNets);
	fprintf(f, "updated=%ld\n", (long)(system_time() / 1000000));
	fclose(f);
	rename(STATUS_FILE ".tmp", STATUS_FILE);
}

/* one line per access point: channel|dBm|security|bssid|ssid   (security: 0 open, 1 WEP, 2 WPA, 3 WPA2) */
static void
write_scan_file(void)
{
	FILE* f = fopen(SCAN_FILE ".tmp", "w");
	char b[20];
	int i, sec;

	if (f == NULL)
		return;
	for (i = 0; i < gNumAp; i++) {
		sec = gAp[i].rsnlen ? 3 : (gAp[i].wpa1 ? 2 : ((gAp[i].capinfo & 0x10) ? 1 : 0));
		mac_str(gAp[i].bssid, b);
		fprintf(f, "%d|%d|%d|%s|%s\n", gAp[i].chan, gAp[i].rssi - 112, sec, b, gAp[i].ssid);
	}
	fclose(f);
	rename(SCAN_FILE ".tmp", SCAN_FILE);
}

/* The panel drops a one-line file /tmp/iwid.cmd:   scan   |   reload   |   connect <ssid>
   `connect` makes that saved network the first one to try, then behaves like `reload`. */
static int
poll_cmd(void)
{
	FILE* f;
	char line[128];
	int n;

	f = fopen(CMD_FILE, "r");
	if (f == NULL)
		return PCMD_NONE;
	line[0] = '\0';
	if (fgets(line, sizeof(line), f) == NULL)
		line[0] = '\0';
	fclose(f);
	remove(CMD_FILE);
	n = (int)strlen(line);
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		line[--n] = '\0';
	if (strcmp(line, "scan") == 0)
		return PCMD_SCAN;
	if (strcmp(line, "reload") == 0)
		return PCMD_RELOAD;
	if (strncmp(line, "connect ", 8) == 0) {
		strncpy(gPrefer, line + 8, sizeof(gPrefer) - 1);
		gPrefer[sizeof(gPrefer) - 1] = '\0';
		return PCMD_RELOAD;
	}
	say("ignoring unknown panel command '%s'", line);
	return PCMD_NONE;
}

/* sleep for `ms` while serving panel commands; returns 1 if a reload/connect was requested */
static int
wait_with_commands(int ms)
{
	bigtime_t t0 = system_time();
	int c;

	while (system_time() - t0 < (bigtime_t)ms * 1000) {
		c = poll_cmd();
		if (c == PCMD_RELOAD)
			return 1;
		if (c == PCMD_SCAN) {
			scan_for_aps();
			write_scan_file();
		}
		snooze(250000);
	}
	return 0;
}

/* ------------------------------ bridge: network stack <-> encrypted air ------------------------------ */

/* run until the link is lost; returns when the caller should reconnect */
static int
bridge(void)
{
	static u8 item[2048], eth[2048];
	static struct iwi_netbuf nb;
	int n, m, busy;
	unsigned long rx = 0, tx = 0, drops = 0;
	bigtime_t lastreport = system_time();
	bigtime_t lastping = system_time();
	bigtime_t lastitem = system_time();
	bigtime_t laststatus = 0, lastcmd = 0;

	if (dhcp_acquire() == 0) {
		apply_address();
		if (gSelfPing)
			self_ping(gRouter);
	}
	gLinked = 1;
	write_status("connected", gLease ? "" : "no address: DHCP did not answer");
	say("bridge running: /dev/net/iwi/0 is live");
	for (;;) {
		busy = 0;
		if (system_time() - laststatus > 2000000) {
			write_status("connected", gLease ? "" : "no address: DHCP did not answer");
			laststatus = system_time();
		}
		if (system_time() - lastcmd > 1000000) {
			int c = poll_cmd();

			lastcmd = system_time();
			if (c == PCMD_RELOAD) {
				say("panel asked to reconnect / reload the saved networks");
				return -3;
			}
			if (c == PCMD_SCAN) {
				/* A SCAN command while associated crashes the card's firmware (SYSASSERT, see the debugging story),
				   so a rescan is a deliberate restart of the card: a few seconds offline, then a fresh scan. */
				say("panel asked for a scan: restarting the card to scan safely");
				return -4;
			}
		}
		if (system_time() - lastitem > 10000000) {
			/* a healthy card sends a notification every fraction of a second; 10 s of silence means it is dead */
			say("the card has been silent for 10 s: restarting it");
			report_card("silent");
			return -2;
		}
		if (gLease && system_time() - gLeaseStart > (bigtime_t)(gLease / 2) * 1000000)
			dhcp_renew();
		if (gSelfPing && system_time() - lastping > 10000000) {
			self_ping(gRouter);
			lastping = system_time();
		}
		n = rx_item(item);
		if (n > 0) {
			busy = 1;
			lastitem = system_time();
			if (item[0] == ITEM_FRAME) {
				if (n > 28 && ((item + 28)[0] & 0x0c) == 0x08) {
					int d = (int)item[17] - 112;

					gCurDbm = (gCurDbm == -100) ? d : (gCurDbm * 3 + d) / 4;
				}
				m = rx_to_ethernet(item, n, eth);
				if (m > 0 && memcmp(eth + 6, gMac, 6) != 0) {
					if (eth[12] == 0x88 && eth[13] == 0x8e) {
						group_rekey(eth + 14, m - 14);
					} else if (gEchoResponder && icmp_answer(eth, m)) {
						/* answered by us; the stack never sees it */
					} else {
						nb.len = m;
						memcpy(nb.data, eth, m);
						ioctl(gFd, IWI_IOC_NETRX, &nb, sizeof(nb));
						rx++;
						if (gVerbose) {
							char a[20], c[20];
							mac_str(eth, a);
							mac_str(eth + 6, c);
							say("air->stack %s <- %s type %02x%02x len %d", a, c, eth[12], eth[13], m);
						}
					}
				} else if (m < 0 && ((item + 28)[0] & 0x0c) == 0x08) {
					drops++;
					if (gVerbose && drops <= 60) {
						const u8* wh = item + 28;
						char a1[20], a2[20], a3[20];
						mac_str(wh + 4, a1); mac_str(wh + 10, a2); mac_str(wh + 16, a3);
						say("rejected frame: fc %02x%02x len %d A1 %s A2 %s A3 %s chan %d", wh[0], wh[1], n - 28, a1, a2, a3, item[12]);
					}
				}
			} else if (item[0] == ITEM_NOTIF && item[12] != 25 && gVerbose) {
				say("notification type %d flags %d len %d data %02x %02x %02x %02x", item[12], item[13],
					item[14] | (item[15] << 8), item[16], item[17], item[18], item[19]);
				if (item[12] == NOTIF_ASSOCIATION && item[16] == 0) {
					say("deassociated by the AP");
					return -1;
				}
			} else if (item[0] == ITEM_NOTIF && item[12] == NOTIF_ASSOCIATION && item[16] == 0) {
				say("deassociated by the AP");
				return -1;
			} else if (item[0] == ITEM_NOTIF && item[12] == NOTIF_AUTHENTICATION && item[16] == 0) {
				say("deauthenticated by the AP");
				return -1;
			}
		}
		nb.len = 0;
		if (ioctl(gFd, IWI_IOC_NETTX, &nb, sizeof(nb)) >= 0 && nb.len >= 14) {
			busy = 1;
			if (send_ethernet(nb.data, nb.len) == 0)
				tx++;
			if (gVerbose) {
				char a[20], c[20];
				mac_str(nb.data, a);
				mac_str(nb.data + 6, c);
				say("stack->air %s <- %s type %02x%02x len %ld", a, c, nb.data[12], nb.data[13], nb.len);
				if ((nb.data[12] == 0x08 && (nb.data[13] == 0x00 || nb.data[13] == 0x06))) {
					static int dumped = 0;
					if (dumped++ < 6)
						hexdump("frame", nb.data, nb.len < 98 ? (int)nb.len : 98);
				}
			}
		}
		if (system_time() - lastreport > 60000000) {
			say("bridge: %lu frames to the stack, %lu from it, %lu undecryptable", rx, tx, drops);
			report_card("periodic");
			lastreport = system_time();
		}
		if (!busy)
			snooze(1500);
	}
}

/* ------------------------------ self test: ARP probe for the gateway ------------------------------ */

static int
parse_ip(const char* s, u8* ip)
{
	int a, b, c, d;

	if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4)
		return -1;
	ip[0] = a; ip[1] = b; ip[2] = c; ip[3] = d;
	return 0;
}

static int
self_test(void)
{
	static u8 eth[64], item[2048], out[2048];
	u8 gw[4];
	int i, n, got = 0, frames = 0, decrypted = 0;
	bigtime_t t0;

	if (parse_ip(gGw, gw) != 0)
		return -1;
	memset(eth, 0xff, 6);			/* DA broadcast */
	memcpy(eth + 6, gMac, 6);
	eth[12] = 0x08; eth[13] = 0x06;		/* ARP */
	eth[14] = 0; eth[15] = 1;		/* hardware: Ethernet */
	eth[16] = 0x08; eth[17] = 0x00;		/* protocol: IPv4 */
	eth[18] = 6; eth[19] = 4;
	eth[20] = 0; eth[21] = 1;		/* request */
	memcpy(eth + 22, gMac, 6);
	memset(eth + 28, 0, 4);			/* sender IP 0.0.0.0: an ARP probe */
	memset(eth + 32, 0, 6);
	memcpy(eth + 38, gw, 4);
	say("self test: ARP probe for %s", gGw);
	for (i = 0; i < 5 && !got; i++) {
		if (send_ethernet(eth, 42) != 0)
			say("send failed");
		t0 = system_time();
		while (system_time() - t0 < 1500000 && !got) {
			n = rx_item(item);
			if (n == 0) {
				snooze(2000);
				continue;
			}
			if (item[0] != ITEM_FRAME)
				continue;
			frames++;
			n = rx_to_ethernet(item, n, out);
			if (n < 0)
				continue;
			decrypted++;
			if (gVerbose || out[12] != 0x08 || out[13] != 0x06) {
				char m1[20], m2[20];
				mac_str(out, m1);
				mac_str(out + 6, m2);
				say("decrypted frame: %s <- %s type %02x%02x len %d", m1, m2, out[12], out[13], n);
			}
			if (out[12] == 0x08 && out[13] == 0x06 && out[20] == 0 && out[21] == 2
					&& memcmp(out + 28, gw, 4) == 0) {
				char m[20];
				mac_str(out + 22, m);
				say("ARP REPLY: %s is at %s  -- encrypted traffic works in both directions", gGw, m);
				got = 1;
			}
		}
	}
	say("self test finished: %d frames seen, %d decrypted, reply %s", frames, decrypted, got ? "YES" : "no");
	return got ? 0 : -1;
}


/* -i: connect, then do nothing but watch the card for 40 s (to find out when and why the firmware dies) */
static void
idle_watch(void)
{
	static u8 item[2048];
	struct iwi_status st;
	bigtime_t t0 = system_time(), lastst = 0;
	int n;

	say("idle watch: no traffic from us");
	while (system_time() - t0 < 40000000) {
		n = rx_item(item);
		if (n == 0)
			snooze(2000);
		if (system_time() - lastst >= 1000000) {
			lastst = system_time();
			memset(&st, 0, sizeof(st));
			ioctl(gFd, IWI_IOC_STATUS, &st, sizeof(st));
			printf("[%6.2f] t+%2d  intr_seen %08lx rx_ridx %lu rx_cur %lu\n", (double)system_time() / 1000000.0,
				(int)((system_time() - t0) / 1000000), st.v[0], st.v[6], st.v[7]);
			fflush(stdout);
			if (st.v[0] & 0x40000000) {
				int e, nn = st.v[13] > 4 ? 4 : (int)st.v[13];
				printf("          FIRMWARE ERROR LOG: base %08lx, %lu entries\n", st.v[12], st.v[13]);
				for (e = 0; e < nn; e++)
					printf("            #%d desc %lu time %lu blink %08lx %08lx link %08lx %08lx data %08lx\n", e,
						st.v[14 + e * 7], st.v[15 + e * 7], st.v[16 + e * 7], st.v[17 + e * 7],
						st.v[18 + e * 7], st.v[19 + e * 7], st.v[20 + e * 7]);
				return;
			}
		}
	}
	say("idle watch finished without a firmware error");
}


static void
usage(void)
{
	printf("iwid - WPA2-PSK supplicant and bridge for the iwi Wi-Fi driver (BeOS ZETA)\n"
		"usage: iwid [options]\n"
		"  -b       bridge mode: connect, get an address by DHCP, then carry /dev/net/iwi/0 traffic until killed (boot mode)\n"
		"  -r       with -b: also replace the default route and write /etc/resolv.conf from the DHCP lease\n"
		"  -v       verbose log (frames, notifications, card status every 5 s)\n"
		"  --scan   only scan and list access points\n"
		"  -t       with -b: self-test, ping the router with our own packets every 10 s\n"
		"  -e       with -b: answer ICMP echo requests ourselves (debugging aid)\n"
		"  -i       connect, then idle for 40 s and watch the card (finds firmware crashes)\n"
		"  without -b: connect once, send an ARP probe to the gateway and exit (smoke test)\n"
		"config: %s  (pairs of ssid= / psk= lines, one pair per saved network; optional gw=, fw=)\n"
		"panel interface: /tmp/iwid.status, /tmp/iwid.scan (written), /tmp/iwid.cmd (read: scan | reload | connect <ssid>)\n", CONF_PATH);
}

/* ------------------------------ main ------------------------------ */

/* upload the firmware, reset and start the card, send the basic configuration */
static int
card_start(void)
{
	struct iwi_up up;
	char b[20];
	int r;

	if (gFwPath[0] == '\0' || upload_firmware(gFwPath) != 0)
		return -1;
	memset(&up, 0, sizeof(up));
	if (ioctl(gFd, IWI_IOC_UP, &up, sizeof(up)) < 0 || up.result != 0) {
		say("card start failed, code %ld", up.result);
		return -1;
	}
	memcpy(gMac, up.mac, 6);
	mac_str(gMac, b);
	say("card is up, MAC %s", b);
	if ((r = setup_card()) != 0) {
		say("card setup failed (%d)", r);
		return -1;
	}
	return 0;
}

/* print the card's registers and, if the firmware crashed, its own error report */
static void
report_card(const char* why)
{
	struct iwi_status st;
	int e, nn;

	memset(&st, 0, sizeof(st));
	if (ioctl(gFd, IWI_IOC_STATUS, &st, sizeof(st)) < 0)
		return;
	say("card status (%s): intr_seen %08lx rst %08lx ctl %08lx io %08lx cmd_ridx %lu rx_ridx %lu rx_cur %lu tx_ridx %lu tx_cur %lu",
		why, st.v[0], st.v[2], st.v[3], st.v[4], st.v[5], st.v[6], st.v[7], st.v[8], st.v[9]);
	if (st.v[0] & 0x40000000) {
		nn = st.v[13] > 4 ? 4 : (int)st.v[13];
		say("FIRMWARE ERROR LOG: base %08lx, %lu entries", st.v[12], st.v[13]);
		for (e = 0; e < nn; e++)
			say("  #%d desc %lu time %lu blink %08lx %08lx link %08lx %08lx data %08lx", e,
				st.v[14 + e * 7], st.v[15 + e * 7], st.v[16 + e * 7], st.v[17 + e * 7],
				st.v[18 + e * 7], st.v[19 + e * 7], st.v[20 + e * 7]);
	}
}


/* 1 if the card's firmware has reported a fatal error since the last start */
static int
card_has_crashed(void)
{
	struct iwi_status st;

	memset(&st, 0, sizeof(st));
	if (ioctl(gFd, IWI_IOC_STATUS, &st, sizeof(st)) < 0)
		return 0;
	return (st.v[0] & 0x40000000) != 0;
}

/* scan, pick an access point of a saved network (the one the panel asked for first, then the strongest),
   associate and run the handshake */
static int
connect_once(void)
{
	u8 order[MAX_AP], netof[MAX_AP];
	int i, j, k, n, ncand = 0, wrongpw = 0, r;
	char b[20];

	gLinked = 0;
	write_status("scanning", "");
	n = scan_for_aps();
	say("scan: %d access points", n);
	write_scan_file();
	for (i = 0; i < gNumAp; i++) {
		if (!gAp[i].rsnlen)
			continue;
		for (k = 0; k < gNumNets; k++) {
			if (strcmp(gAp[i].ssid, gNets[k].ssid) == 0)
				break;
		}
		if (k < gNumNets) {
			order[ncand] = i;
			netof[ncand] = k;
			ncand++;
		}
	}
	if (ncand == 0) {
		if (gNumNets == 0)
			write_status("no-network", "No saved network yet: pick one in the Wi-Fi panel.");
		else
			write_status("no-network", "None of the saved networks is in range.");
		say("no access point of a saved network (WPA2) found");
		return -1;
	}
	/* preferred network first, then strongest signal */
	for (i = 0; i < ncand; i++) {
		for (j = i + 1; j < ncand; j++) {
			int pi = gPrefer[0] && strcmp(gNets[netof[i]].ssid, gPrefer) == 0;
			int pj = gPrefer[0] && strcmp(gNets[netof[j]].ssid, gPrefer) == 0;

			if ((pj && !pi) || (pi == pj && gAp[order[j]].rssi > gAp[order[i]].rssi)) {
				u8 t = order[i], t2 = netof[i];
				order[i] = order[j]; netof[i] = netof[j];
				order[j] = t; netof[j] = t2;
			}
		}
	}
	for (i = 0; i < ncand; i++) {
		ap_t* a = &gAp[order[i]];
		net_t* nt = &gNets[netof[i]];

		strcpy(gSsid, nt->ssid);
		if (!nt->have_pmk) {
			say("deriving the key for '%s' (PBKDF2, 4096 rounds)...", nt->ssid);
			pbkdf2_sha1(nt->psk, (const u8*)nt->ssid, (int)strlen(nt->ssid), 4096, nt->pmk, 32);
			nt->have_pmk = 1;
		}
		memcpy(gPmk, nt->pmk, 32);
		mac_str(a->bssid, b);
		say("trying '%s' %s on channel %d", nt->ssid, b, a->chan);
		write_status("connecting", "");
		memcpy(gBssid, a->bssid, 6);
		if (associate(a) != 0)
			continue;
		r = do_handshake();
		if (r == -2)
			wrongpw = 1;
		if (r != 0)
			continue;
		gCurChan = a->chan;
		gCurDbm = a->rssi - 112;
		gPrefer[0] = '\0';
		return 0;
	}
	{
		char msg[160];

		sprintf(msg, wrongpw ? "Could not connect to '%s': the password is probably wrong." : "Could not connect to '%s'.", gSsid);
		write_status("failed", msg);
	}
	say("could not complete the WPA2 connection");
	return -1;
}

int
main(int argc, char** argv)
{
	int i, n, r, scanonly = 0;
	char b[20];

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage();
			return 0;
		} else if (strcmp(argv[i], "-v") == 0) {
			gVerbose = 1;
			gLogMgmt = 1;
		}
		else if (strcmp(argv[i], "--scan") == 0)
			scanonly = 1;
		else if (strcmp(argv[i], "-b") == 0)
			gBridge = 1;
		else if (strcmp(argv[i], "-r") == 0)
			gTakeOver = 1;
		else if (strcmp(argv[i], "-i") == 0)
			gIdle = 1;
		else if (strcmp(argv[i], "-e") == 0)
			gEchoResponder = 1;
		else if (strcmp(argv[i], "-t") == 0)
			gSelfPing = 1;
	}
	read_config(CONF_PATH);

	gFd = open(IWI_DEVICE, O_RDWR);
	if (gFd < 0) {
		printf("cannot open %s (is the iwi driver installed? try: rescan iwi)\n", IWI_DEVICE);
		return 1;
	}
	if (card_start() != 0)
		return 1;
	if (scanonly) {
		n = scan_for_aps();
		say("scan: %d access points", n);
		for (i = 0; i < gNumAp; i++) {
			mac_str(gAp[i].bssid, b);
			say("  ch%2d %4d dBm %s %s%s", gAp[i].chan, gAp[i].rssi - 112, b, gAp[i].ssid, gAp[i].rsnlen ? "  [RSN]" : "");
		}
		goto done;
	}

	if (gBridge)
		write_status("starting", "");

	for (;;) {
		gLinked = 0;
		r = connect_once();
		if (r != 0) {
			if (!gBridge)
				break;
			if (card_has_crashed()) {
				say("the card's firmware has crashed: restarting the card");
				report_card("crashed while not connected");
				write_status("restarting", "The Wi-Fi card stopped answering; restarting it.");
				while (card_start() != 0)
					snooze(5000000);
				continue;
			}
			if (wait_with_commands(4000)) {
				read_config(CONF_PATH);
				/* a command that arrived while we are not associated: scanning is safe, but start clean anyway */
			}
			continue;
		}
		if (gIdle) {
			idle_watch();
			break;
		}
		if (!gBridge) {
			self_test();
			break;
		}
		r = bridge();
		gLinked = 0;
		if (r == -3) {
			/* the panel changed the saved networks or asked for one: re-read them, then restart the card
			   (scanning while still associated crashes the firmware) and reconnect */
			read_config(CONF_PATH);
			write_status("scanning", "");
			while (card_start() != 0)
				snooze(5000000);
			continue;
		}
		if (r == -4) {
			write_status("scanning", "");
			while (card_start() != 0)
				snooze(5000000);
			continue;
		}
		say("link lost, reconnecting");
		write_status("reconnecting", "");
		snooze(2000000);
		if (r == -2) {
			/* the card died: full restart (firmware reload) */
			write_status("restarting", "The Wi-Fi card stopped answering; restarting it.");
			while (card_start() != 0)
				snooze(5000000);
		}
	}
done:
	ioctl(gFd, IWI_IOC_DOWN, NULL, 0);
	close(gFd);
	return 0;
}
