/* wificonf.h - editing the text of /boot/home/config/settings/iwi.conf, with no Be API (so it can be tested anywhere).
 *
 * The file is a list of lines.  An "ssid=NAME" line followed directly by a "psk=PASSWORD" line is one saved network
 * (this is what iwid reads).  Every other line (comments, fw=, gw=, ...) is kept exactly as it is.
 * All functions take the file's text and write the new text to `out`; they return the new length, or -1 if
 * `out` is too small.  Output lines always end with '\n' and never contain '\r'.
 * Copyright (c) 2026 Vihaan Nathan, ISC licence.
 */
#ifndef WIFICONF_H
#define WIFICONF_H

#include <string.h>

/* the next line of `text` starting at *pos: its start and length (without \n and \r); returns 0 at the end */
static int
wc_next_line(const char* text, int* pos, const char** start, int* len)
{
	int p = *pos, e;

	if (text[p] == '\0')
		return 0;
	e = p;
	while (text[e] != '\0' && text[e] != '\n')
		e++;
	*start = text + p;
	*len = e - p;
	while (*len > 0 && (*start)[*len - 1] == '\r')
		(*len)--;
	*pos = (text[e] == '\n') ? e + 1 : e;
	return 1;
}

static int
wc_line_is_ssid(const char* line, int len, const char* ssid)
{
	int sl = (int)strlen(ssid);

	return len == 5 + sl && strncmp(line, "ssid=", 5) == 0 && strncmp(line + 5, ssid, sl) == 0;
}

static int
wc_line_is_psk(const char* line, int len)
{
	return len >= 4 && strncmp(line, "psk=", 4) == 0;
}

static int
wc_append(char* out, int outsize, int* used, const char* s, int len)
{
	if (*used + len + 1 > outsize)
		return -1;
	memcpy(out + *used, s, len);
	*used += len;
	out[*used] = '\0';
	return 0;
}

static int
wc_append_line(char* out, int outsize, int* used, const char* s, int len)
{
	if (wc_append(out, outsize, used, s, len) < 0 || wc_append(out, outsize, used, "\n", 1) < 0)
		return -1;
	return 0;
}

/* 1 if the file has a saved network called ssid */
static int
conf_has_ssid(const char* text, const char* ssid)
{
	int pos = 0, len;
	const char* line;

	while (wc_next_line(text, &pos, &line, &len)) {
		if (wc_line_is_ssid(line, len, ssid))
			return 1;
	}
	return 0;
}

/* add the network, or replace the password of an existing one (its position in the file is kept) */
static int
conf_set_network(const char* text, const char* ssid, const char* psk, char* out, int outsize)
{
	int pos = 0, len, used = 0, done = 0, skip_psk = 0;
	const char* line;

	out[0] = '\0';
	while (wc_next_line(text, &pos, &line, &len)) {
		if (skip_psk) {
			skip_psk = 0;
			if (wc_line_is_psk(line, len))
				continue;
		}
		if (wc_line_is_ssid(line, len, ssid)) {
			if (done)			/* a duplicate entry of the same network: drop it */
				{ skip_psk = 1; continue; }
			if (wc_append_line(out, outsize, &used, line, len) < 0
					|| wc_append(out, outsize, &used, "psk=", 4) < 0
					|| wc_append_line(out, outsize, &used, psk, (int)strlen(psk)) < 0)
				return -1;
			done = 1;
			skip_psk = 1;
			continue;
		}
		if (wc_append_line(out, outsize, &used, line, len) < 0)
			return -1;
	}
	if (!done) {
		if (wc_append(out, outsize, &used, "ssid=", 5) < 0 || wc_append_line(out, outsize, &used, ssid, (int)strlen(ssid)) < 0
				|| wc_append(out, outsize, &used, "psk=", 4) < 0
				|| wc_append_line(out, outsize, &used, psk, (int)strlen(psk)) < 0)
			return -1;
	}
	return used;
}

/* remove the network (its ssid line and the psk line right after it); everything else is kept */
static int
conf_forget_network(const char* text, const char* ssid, char* out, int outsize)
{
	int pos = 0, len, used = 0, skip_psk = 0;
	const char* line;

	out[0] = '\0';
	while (wc_next_line(text, &pos, &line, &len)) {
		if (skip_psk) {
			skip_psk = 0;
			if (wc_line_is_psk(line, len))
				continue;
		}
		if (wc_line_is_ssid(line, len, ssid)) {
			skip_psk = 1;
			continue;
		}
		if (wc_append_line(out, outsize, &used, line, len) < 0)
			return -1;
	}
	return used;
}

#endif
