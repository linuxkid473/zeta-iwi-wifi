/* iwi_ioctl.h - interface between the iwi kernel driver (/dev/misc/iwi/0) and the userland daemon */
#ifndef IWI_IOCTL_H
#define IWI_IOCTL_H

#define IWI_DEVICE		"/dev/misc/iwi/0"

#define IWI_IOC_FWCHUNK		20001	/* struct iwi_fwchunk: upload (part of) the firmware file (as in OpenBSD iwi-bss) */
#define IWI_IOC_UP		20002	/* struct iwi_up: reset the card, load firmware, start it */
#define IWI_IOC_CMD		20003	/* struct iwi_cmdio: send one firmware command and wait for it to complete */
#define IWI_IOC_DOWN		20004	/* stop the card */
#define IWI_IOC_STATUS		20007	/* struct iwi_status: debugging snapshot of card registers and ring pointers */
#define IWI_IOC_NETRX		20005	/* struct iwi_netbuf: hand an Ethernet frame to the network stack (/dev/net/iwi/0) */
#define IWI_IOC_NETTX		20006	/* struct iwi_netbuf: take the next Ethernet frame the stack wants to send (len 0 = none) */

/* read(): one received item per call (0 bytes = nothing waiting). First byte: 9 = 802.11 frame, 3 = notification.
     frame:        [0..3] iwi_hdr, [4..27] iwi_frame (chan @ +12, rssi_dbm @ +17, len @ +26), then the 802.11 frame from +28
     notification: [0..3] iwi_hdr, [4..15] iwi_notif (type @ +12, len @ +14), data from +16
   write(): one complete 802.11 data frame (24 or 26 byte header + payload, already encrypted if needed) */

struct iwi_fwchunk {
	unsigned long	offset;
	unsigned long	len;
	unsigned char	data[4096];
};

struct iwi_up {
	long		result;		/* 0 = firmware running, else a negative code (see driver) */
	unsigned char	mac[6];
	unsigned char	pad[2];
};

struct iwi_status {
	unsigned long	v[48];	/* [0..11] intr_seen intr rst ctl io cmd_ridx rx_ridx rx_cur tx_ridx tx_cur rx_widx intr_mask;
			   [12] error log base, [13] error count, [14..] up to 4 x (desc time blink1 blink2 link1 link2 data) */
};

struct iwi_netbuf {
	long		len;
	unsigned char	data[1536];
};

struct iwi_cmdio {
	unsigned char	type;
	unsigned char	len;
	unsigned char	pad[2];
	long		result;
	unsigned char	data[120];
};

#endif
