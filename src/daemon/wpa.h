/* wpa.h - the crypto WPA2-PSK (CCMP) needs: SHA-1, HMAC-SHA1, PBKDF2, PRF, AES-128, key unwrap, CCM.
   Plain C89, 32-bit "unsigned int", no libraries: builds the same on ZETA's gcc 2.95 and on the Mac. */
#ifndef WPA_H
#define WPA_H

typedef unsigned char u8;
typedef unsigned int u32;

typedef struct {
	u32 h[5];
	u32 len_lo, len_hi;
	u8 buf[64];
	int n;
} sha1_ctx;

void sha1_init(sha1_ctx* c);
void sha1_update(sha1_ctx* c, const u8* data, u32 len);
void sha1_final(sha1_ctx* c, u8 out[20]);
void sha1(const u8* data, u32 len, u8 out[20]);

void hmac_sha1(const u8* key, int keylen, const u8* data, int len, u8 out[20]);
/* IEEE 802.11i PSK: PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32) */
void pbkdf2_sha1(const char* pass, const u8* salt, int saltlen, int iterations, u8* out, int outlen);
/* IEEE 802.11i PRF: out = first outlen bytes of HMAC-SHA1(k, label || 0 || data || counter)... */
void wpa_prf(const u8* key, int keylen, const char* label, const u8* data, int datalen, u8* out, int outlen);

typedef struct {
	u8 rk[11][16];
} aes_ctx;

void aes_set_key(aes_ctx* c, const u8 key[16]);
void aes_encrypt(const aes_ctx* c, const u8 in[16], u8 out[16]);
void aes_decrypt(const aes_ctx* c, const u8 in[16], u8 out[16]);
/* RFC 3394 AES key unwrap with a 128-bit KEK. in = (n+1)*8 bytes, out = n*8 bytes. returns 0 if the integrity check passes. */
int aes_unwrap(const u8 kek[16], const u8* in, int inlen, u8* out);

/* CCMP (802.11i): CCM with M=8, L=2.  nonce is 13 bytes.
   encrypt: data (len bytes) is encrypted in place and an 8 byte MIC is written after it.
   decrypt: data (len bytes, NOT including the 8 byte MIC, which is passed in mic) is decrypted in place; returns 0 if the MIC verifies. */
void ccm_encrypt(const aes_ctx* c, const u8 nonce[13], const u8* aad, int aadlen, u8* data, int len, u8 mic[8]);
int ccm_decrypt(const aes_ctx* c, const u8 nonce[13], const u8* aad, int aadlen, u8* data, int len, const u8 mic[8]);

#endif
