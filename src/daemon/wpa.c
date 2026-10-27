/* wpa.c - see wpa.h.  Copyright (c) 2026 Vihaan Nathan, ISC licence (see LICENSE).  Written from the public algorithm descriptions (FIPS 180-1, RFC 2104, RFC 2898,
   FIPS 197, RFC 3394, RFC 3610, IEEE 802.11i). */
#include <string.h>
#include "wpa.h"

/* ------------------------------ SHA-1 ------------------------------ */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void
sha1_block(u32 h[5], const u8* p)
{
	u32 w[80], a, b, c, d, e, f, k, t;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) | ((u32)p[i * 4 + 2] << 8) | p[i * 4 + 3];
	for (i = 16; i < 80; i++)
		w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
		else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
		else { f = b ^ c ^ d; k = 0xca62c1d6; }
		t = ROL(a, 5) + f + e + k + w[i];
		e = d; d = c; c = ROL(b, 30); b = a; a = t;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void
sha1_init(sha1_ctx* c)
{
	c->h[0] = 0x67452301; c->h[1] = 0xefcdab89; c->h[2] = 0x98badcfe;
	c->h[3] = 0x10325476; c->h[4] = 0xc3d2e1f0;
	c->len_lo = c->len_hi = 0;
	c->n = 0;
}

void
sha1_update(sha1_ctx* c, const u8* data, u32 len)
{
	u32 i;

	for (i = 0; i < len; i++) {
		c->buf[c->n++] = data[i];
		if (c->n == 64) {
			sha1_block(c->h, c->buf);
			c->n = 0;
		}
	}
	c->len_lo += len;
	if (c->len_lo < len)
		c->len_hi++;
}

void
sha1_final(sha1_ctx* c, u8 out[20])
{
	u32 lo = c->len_lo << 3, hi = (c->len_hi << 3) | (c->len_lo >> 29);
	u8 pad = 0x80, zero = 0, lenb[8];
	int i;

	sha1_update(c, &pad, 1);
	while (c->n != 56)
		sha1_update(c, &zero, 1);
	for (i = 0; i < 4; i++) {
		lenb[i] = (u8)(hi >> (24 - 8 * i));
		lenb[4 + i] = (u8)(lo >> (24 - 8 * i));
	}
	sha1_update(c, lenb, 8);
	for (i = 0; i < 20; i++)
		out[i] = (u8)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

void
sha1(const u8* data, u32 len, u8 out[20])
{
	sha1_ctx c;

	sha1_init(&c);
	sha1_update(&c, data, len);
	sha1_final(&c, out);
}

/* ------------------------------ HMAC / PBKDF2 / PRF ------------------------------ */

typedef struct {
	sha1_ctx inner, outer;
} hmac_ctx;

static void
hmac_init(hmac_ctx* h, const u8* key, int keylen)
{
	u8 k[64], pad[64];
	int i;

	memset(k, 0, sizeof(k));
	if (keylen > 64)
		sha1(key, keylen, k);
	else
		memcpy(k, key, keylen);
	for (i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x36;
	sha1_init(&h->inner);
	sha1_update(&h->inner, pad, 64);
	for (i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x5c;
	sha1_init(&h->outer);
	sha1_update(&h->outer, pad, 64);
}

/* compute HMAC from a pre-keyed context (copies it) */
static void
hmac_run(const hmac_ctx* h, const u8* data, int len, u8 out[20])
{
	sha1_ctx in = h->inner, o = h->outer;
	u8 tmp[20];

	sha1_update(&in, data, len);
	sha1_final(&in, tmp);
	sha1_update(&o, tmp, 20);
	sha1_final(&o, out);
}

void
hmac_sha1(const u8* key, int keylen, const u8* data, int len, u8 out[20])
{
	hmac_ctx h;

	hmac_init(&h, key, keylen);
	hmac_run(&h, data, len, out);
}

void
pbkdf2_sha1(const char* pass, const u8* salt, int saltlen, int iterations, u8* out, int outlen)
{
	hmac_ctx h;
	u8 s[64 + 4], u[20], t[20];
	u32 block = 1;
	int done = 0, i, j, n;

	hmac_init(&h, (const u8*)pass, (int)strlen(pass));
	while (done < outlen) {
		memcpy(s, salt, saltlen);
		s[saltlen] = (u8)(block >> 24); s[saltlen + 1] = (u8)(block >> 16);
		s[saltlen + 2] = (u8)(block >> 8); s[saltlen + 3] = (u8)block;
		hmac_run(&h, s, saltlen + 4, u);
		memcpy(t, u, 20);
		for (i = 1; i < iterations; i++) {
			hmac_run(&h, u, 20, u);
			for (j = 0; j < 20; j++)
				t[j] ^= u[j];
		}
		n = outlen - done < 20 ? outlen - done : 20;
		memcpy(out + done, t, n);
		done += n;
		block++;
	}
}

void
wpa_prf(const u8* key, int keylen, const char* label, const u8* data, int datalen, u8* out, int outlen)
{
	u8 buf[128], r[20];
	int llen = (int)strlen(label), done = 0, n;
	u8 counter = 0;
	hmac_ctx h;

	memcpy(buf, label, llen);
	buf[llen] = 0;
	memcpy(buf + llen + 1, data, datalen);
	hmac_init(&h, key, keylen);
	while (done < outlen) {
		buf[llen + 1 + datalen] = counter++;
		hmac_run(&h, buf, llen + 1 + datalen + 1, r);
		n = outlen - done < 20 ? outlen - done : 20;
		memcpy(out + done, r, n);
		done += n;
	}
}

/* ------------------------------ AES-128 ------------------------------ */

static u8 sbox[256], isbox[256];
static int aes_ready = 0;

static u8
xtime(u8 x)
{
	return (u8)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static u8
gmul(u8 a, u8 b)
{
	u8 r = 0;

	while (b) {
		if (b & 1)
			r ^= a;
		a = xtime(a);
		b >>= 1;
	}
	return r;
}

static u8
rotl8(u8 x, int n)
{
	return (u8)((x << n) | (x >> (8 - n)));
}

static void
aes_init_tables(void)
{
	u8 p = 1, q = 1, x;
	int i;

	if (aes_ready)
		return;
	/* standard generation of the S-box from the multiplicative inverse in GF(2^8) */
	do {
		p = (u8)(p ^ (u8)(p << 1) ^ ((p & 0x80) ? 0x1b : 0));		/* p *= 3 */
		q ^= (u8)(q << 1);						/* q /= 3 */
		q ^= (u8)(q << 2);
		q ^= (u8)(q << 4);
		if (q & 0x80)
			q ^= 0x09;
		x = (u8)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
		sbox[p] = (u8)(x ^ 0x63);
	} while (p != 1);
	sbox[0] = 0x63;
	for (i = 0; i < 256; i++)
		isbox[sbox[i]] = (u8)i;
	aes_ready = 1;
}

void
aes_set_key(aes_ctx* c, const u8 key[16])
{
	int i, j;
	u8 rcon = 1, t[4];

	aes_init_tables();
	memcpy(c->rk[0], key, 16);
	for (i = 1; i < 11; i++) {
		memcpy(t, c->rk[i - 1] + 12, 4);
		/* RotWord + SubWord + Rcon */
		{
			u8 tmp = t[0];
			t[0] = sbox[t[1]] ^ rcon;
			t[1] = sbox[t[2]];
			t[2] = sbox[t[3]];
			t[3] = sbox[tmp];
		}
		rcon = xtime(rcon);
		for (j = 0; j < 4; j++)
			c->rk[i][j] = c->rk[i - 1][j] ^ t[j];
		for (j = 4; j < 16; j++)
			c->rk[i][j] = c->rk[i - 1][j] ^ c->rk[i][j - 4];
	}
}

static void
add_round_key(u8 s[16], const u8 rk[16])
{
	int i;

	for (i = 0; i < 16; i++)
		s[i] ^= rk[i];
}

void
aes_encrypt(const aes_ctx* c, const u8 in[16], u8 out[16])
{
	u8 s[16], t[16];
	int r, i;

	memcpy(s, in, 16);
	add_round_key(s, c->rk[0]);
	for (r = 1; r <= 10; r++) {
		/* SubBytes + ShiftRows (state is column-major: s[4*col+row]) */
		for (i = 0; i < 16; i++)
			t[i] = sbox[s[(i + 4 * (i % 4)) % 16]];
		if (r != 10) {
			for (i = 0; i < 4; i++) {
				u8 a0 = t[4 * i], a1 = t[4 * i + 1], a2 = t[4 * i + 2], a3 = t[4 * i + 3];
				s[4 * i]     = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
				s[4 * i + 1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
				s[4 * i + 2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
				s[4 * i + 3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
			}
		} else
			memcpy(s, t, 16);
		add_round_key(s, c->rk[r]);
	}
	memcpy(out, s, 16);
}

void
aes_decrypt(const aes_ctx* c, const u8 in[16], u8 out[16])
{
	u8 s[16], t[16];
	int r, i;

	memcpy(s, in, 16);
	add_round_key(s, c->rk[10]);
	for (r = 9; r >= 0; r--) {
		/* InvShiftRows + InvSubBytes */
		for (i = 0; i < 16; i++)
			t[(i + 4 * (i % 4)) % 16] = isbox[s[i]];
		add_round_key(t, c->rk[r]);
		if (r != 0) {
			for (i = 0; i < 4; i++) {
				u8 a0 = t[4 * i], a1 = t[4 * i + 1], a2 = t[4 * i + 2], a3 = t[4 * i + 3];
				s[4 * i]     = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
				s[4 * i + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
				s[4 * i + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
				s[4 * i + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
			}
		} else
			memcpy(s, t, 16);
	}
	memcpy(out, s, 16);
}

/* ------------------------------ RFC 3394 unwrap ------------------------------ */

int
aes_unwrap(const u8 kek[16], const u8* in, int inlen, u8* out)
{
	aes_ctx c;
	u8 a[8], b[16];
	int n = inlen / 8 - 1, i, j, k;
	u32 t;

	if (inlen < 24 || (inlen % 8) != 0)
		return -1;
	aes_set_key(&c, kek);
	memcpy(a, in, 8);
	memcpy(out, in + 8, n * 8);
	for (j = 5; j >= 0; j--) {
		for (i = n; i >= 1; i--) {
			t = (u32)(n * j + i);
			memcpy(b, a, 8);
			b[7] ^= (u8)t;
			b[6] ^= (u8)(t >> 8);
			b[5] ^= (u8)(t >> 16);
			b[4] ^= (u8)(t >> 24);
			memcpy(b + 8, out + (i - 1) * 8, 8);
			aes_decrypt(&c, b, b);
			memcpy(a, b, 8);
			memcpy(out + (i - 1) * 8, b + 8, 8);
		}
	}
	for (k = 0; k < 8; k++) {
		if (a[k] != 0xa6)
			return -1;
	}
	return 0;
}

/* ------------------------------ CCM (M = 8, L = 2) ------------------------------ */

static void
ccm_mac(const aes_ctx* c, const u8 nonce[13], const u8* aad, int aadlen, const u8* msg, int len, u8 x[16])
{
	u8 blk[16];
	int i, n;

	/* B0 */
	blk[0] = (aadlen > 0 ? 0x40 : 0) | (((8 - 2) / 2) << 3) | (2 - 1);
	memcpy(blk + 1, nonce, 13);
	blk[14] = (u8)(len >> 8);
	blk[15] = (u8)len;
	aes_encrypt(c, blk, x);
	if (aadlen > 0) {
		int pos = 0;
		memset(blk, 0, 16);
		blk[0] = (u8)(aadlen >> 8);
		blk[1] = (u8)aadlen;
		n = aadlen < 14 ? aadlen : 14;
		memcpy(blk + 2, aad, n);
		pos = n;
		for (;;) {
			for (i = 0; i < 16; i++)
				blk[i] ^= x[i];
			aes_encrypt(c, blk, x);
			if (pos >= aadlen)
				break;
			memset(blk, 0, 16);
			n = aadlen - pos < 16 ? aadlen - pos : 16;
			memcpy(blk, aad + pos, n);
			pos += n;
		}
	}
	for (n = 0; n < len; n += 16) {
		int m = len - n < 16 ? len - n : 16;
		memset(blk, 0, 16);
		memcpy(blk, msg + n, m);
		for (i = 0; i < 16; i++)
			blk[i] ^= x[i];
		aes_encrypt(c, blk, x);
	}
}

static void
ccm_ctr(const aes_ctx* c, const u8 nonce[13], int counter, u8 s[16])
{
	u8 a[16];

	a[0] = 2 - 1;
	memcpy(a + 1, nonce, 13);
	a[14] = (u8)(counter >> 8);
	a[15] = (u8)counter;
	aes_encrypt(c, a, s);
}

void
ccm_encrypt(const aes_ctx* c, const u8 nonce[13], const u8* aad, int aadlen, u8* data, int len, u8 mic[8])
{
	u8 x[16], s[16];
	int n, i, ctr = 1;

	ccm_mac(c, nonce, aad, aadlen, data, len, x);
	ccm_ctr(c, nonce, 0, s);
	for (i = 0; i < 8; i++)
		mic[i] = x[i] ^ s[i];
	for (n = 0; n < len; n += 16, ctr++) {
		int m = len - n < 16 ? len - n : 16;
		ccm_ctr(c, nonce, ctr, s);
		for (i = 0; i < m; i++)
			data[n + i] ^= s[i];
	}
}

int
ccm_decrypt(const aes_ctx* c, const u8 nonce[13], const u8* aad, int aadlen, u8* data, int len, const u8 mic[8])
{
	u8 x[16], s[16];
	int n, i, ctr = 1, bad = 0;

	for (n = 0; n < len; n += 16, ctr++) {
		int m = len - n < 16 ? len - n : 16;
		ccm_ctr(c, nonce, ctr, s);
		for (i = 0; i < m; i++)
			data[n + i] ^= s[i];
	}
	ccm_mac(c, nonce, aad, aadlen, data, len, x);
	ccm_ctr(c, nonce, 0, s);
	for (i = 0; i < 8; i++)
		bad |= (x[i] ^ s[i]) ^ mic[i];
	return bad ? -1 : 0;
}
