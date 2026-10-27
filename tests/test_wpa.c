/* test_wpa.c - checks the WPA2 crypto against published / independently computed vectors.
   Builds and runs on any Unix (and on ZETA):   make test
   Every vector is either from a standard (FIPS 180-1, RFC 2202, IEEE 802.11i, FIPS 197, RFC 3394)
   or was computed with Python's hashlib / hmac / cryptography packages. */
#include <stdio.h>
#include <string.h>
#include "../src/daemon/wpa.h"

static int failures = 0;

static int
unhex(const char* s, u8* out)
{
	int n = (int)strlen(s) / 2, i;

	for (i = 0; i < n; i++) {
		unsigned v;
		sscanf(s + 2 * i, "%2x", &v);
		out[i] = (u8)v;
	}
	return n;
}

static void
check(const char* name, const u8* got, int len, const char* want_hex)
{
	u8 want[128];
	int n = unhex(want_hex, want);

	if (n != len || memcmp(got, want, len) != 0) {
		int i;
		printf("FAIL %-22s got ", name);
		for (i = 0; i < len; i++)
			printf("%02x", got[i]);
		printf("\n     %-22s want %s\n", "", want_hex);
		failures++;
	} else
		printf("ok   %s\n", name);
}

int
main(void)
{
	u8 out[64], k[16], in[64], nonce[13], aad[40], data[100], plain[100], mic[8];
	aes_ctx c;
	int n, i;

	sha1((const u8*)"abc", 3, out);
	check("SHA-1(abc)", out, 20, "a9993e364706816aba3e25717850c26c9cd0d89d");
	hmac_sha1((const u8*)"Jefe", 4, (const u8*)"what do ya want for nothing?", 28, out);
	check("HMAC-SHA1 RFC2202 #2", out, 20, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
	pbkdf2_sha1("password", (const u8*)"IEEE", 4, 4096, out, 32);
	check("PBKDF2 802.11i #1", out, 32, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
	pbkdf2_sha1("ThisIsAPassword", (const u8*)"ThisIsASSID", 11, 4096, out, 32);
	check("PBKDF2 802.11i #2", out, 32, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af");
	wpa_prf((const u8*)"0123456789abcdef", 16, "Pairwise key expansion", (const u8*)"datadatadata", 12, out, 48);
	check("802.11i PRF-384", out, 48,
		"6acb726e1d53f9dcc7f57d6e2cfd852a8c9cc5ebfcd6e2c2efdc1ff6c2ea88c356e62a9e74f8b55d0d5755ed3ab5a8b5");

	unhex("000102030405060708090a0b0c0d0e0f", k);
	unhex("00112233445566778899aabbccddeeff", in);
	aes_set_key(&c, k);
	aes_encrypt(&c, in, out);
	check("AES-128 FIPS-197", out, 16, "69c4e0d86a7b0430d8cdb78070b4c55a");
	aes_decrypt(&c, out, plain);
	check("AES-128 decrypt", plain, 16, "00112233445566778899aabbccddeeff");

	n = unhex("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", in);
	if (aes_unwrap(k, in, n, out) != 0) {
		printf("FAIL AES key unwrap RFC3394 (integrity check)\n");
		failures++;
	} else
		check("AES unwrap RFC3394", out, 16, "00112233445566778899aabbccddeeff");

	for (i = 0; i < 13; i++)
		nonce[i] = (u8)(i * 7 + 1);
	for (i = 0; i < 22; i++)
		aad[i] = (u8)(i * 3 + 5);
	for (i = 0; i < 61; i++)
		data[i] = (u8)(i * 11 + 2);
	memcpy(plain, data, 61);
	ccm_encrypt(&c, nonce, aad, 22, data, 61, mic);
	check("CCM ciphertext", data, 61,
		"54643d829dd2181a9d7a9dfce4aaba6e634126f2002f24013f0ba5c6c24bab1a4b7805f6eca80f87f8ffe7cbe934b9555c247026de791bbb203da38b9c");
	check("CCM MIC", mic, 8, "b4d2b181f6c229ec");
	if (ccm_decrypt(&c, nonce, aad, 22, data, 61, mic) != 0 || memcmp(data, plain, 61) != 0) {
		printf("FAIL CCM round trip\n");
		failures++;
	} else
		printf("ok   CCM round trip\n");
	ccm_encrypt(&c, nonce, aad, 22, data, 61, mic);
	mic[3] ^= 1;
	if (ccm_decrypt(&c, nonce, aad, 22, data, 61, mic) == 0) {
		printf("FAIL CCM accepted a corrupted MIC\n");
		failures++;
	} else
		printf("ok   CCM rejects a corrupted MIC\n");

	printf("%s (%d failure%s)\n", failures ? "TESTS FAILED" : "all tests passed", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
