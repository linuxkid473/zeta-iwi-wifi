/* test_wificonf.c - tests for wificonf.h.   cc -o test_wificonf test_wificonf.c && ./test_wificonf */
#include <stdio.h>
#include <string.h>
#include "wificonf.h"

static int failures = 0;

static void
expect(const char* name, const char* got, const char* want)
{
	if (strcmp(got, want) != 0) {
		printf("FAIL %s\n--- got:\n%s--- want:\n%s---\n", name, got, want);
		failures++;
	} else
		printf("ok   %s\n", name);
}

int
main(void)
{
	char out[2048];
	int n;
	const char* two = "# my networks\nssid=Home\npsk=homepass1\nssid=Work\npsk=workpass1\nfw=/boot/home/x\n";

	n = conf_set_network("", "Home", "secret123", out, sizeof(out));
	expect("add to an empty file", out, "ssid=Home\npsk=secret123\n");
	if (n != (int)strlen(out)) { printf("FAIL length\n"); failures++; }

	conf_set_network("ssid=A\npsk=aaaaaaaa\n", "B", "bbbbbbbb", out, sizeof(out));
	expect("add a second network", out, "ssid=A\npsk=aaaaaaaa\nssid=B\npsk=bbbbbbbb\n");

	conf_set_network(two, "Work", "newpass99", out, sizeof(out));
	expect("replace a password, keep order and other lines", out,
		"# my networks\nssid=Home\npsk=homepass1\nssid=Work\npsk=newpass99\nfw=/boot/home/x\n");

	conf_set_network(two, "Cafe", "cafepass1", out, sizeof(out));
	expect("add keeps comments and fw=", out,
		"# my networks\nssid=Home\npsk=homepass1\nssid=Work\npsk=workpass1\nfw=/boot/home/x\nssid=Cafe\npsk=cafepass1\n");

	conf_set_network("ssid=Home\n", "Home", "secret123", out, sizeof(out));
	expect("an ssid line without psk gets one", out, "ssid=Home\npsk=secret123\n");

	conf_set_network("ssid=Home\npsk=old\nssid=Home\npsk=dup\n", "Home", "newnewnew", out, sizeof(out));
	expect("duplicate entries collapse into one", out, "ssid=Home\npsk=newnewnew\n");

	conf_set_network("ssid=Home\r\npsk=old\r\n", "Home", "newnewnew", out, sizeof(out));
	expect("CRLF input is accepted and normalised", out, "ssid=Home\npsk=newnewnew\n");

	conf_set_network("ssid=Home\npsk=old", "Home", "newnewnew", out, sizeof(out));
	expect("no trailing newline", out, "ssid=Home\npsk=newnewnew\n");

	conf_set_network("ssid=Home Net\npsk=pass word 1\n", "Home Net", "pass word 2", out, sizeof(out));
	expect("spaces in names and passwords", out, "ssid=Home Net\npsk=pass word 2\n");

	conf_set_network("ssid=Home2\npsk=keepme123\n", "Home", "secret123", out, sizeof(out));
	expect("a name that merely starts the same is a different network", out,
		"ssid=Home2\npsk=keepme123\nssid=Home\npsk=secret123\n");

	conf_forget_network(two, "Home", out, sizeof(out));
	expect("forget the first", out, "# my networks\nssid=Work\npsk=workpass1\nfw=/boot/home/x\n");
	conf_forget_network(two, "Work", out, sizeof(out));
	expect("forget the last network", out, "# my networks\nssid=Home\npsk=homepass1\nfw=/boot/home/x\n");
	conf_forget_network(two, "Nope", out, sizeof(out));
	expect("forget an unknown name changes nothing", out, two);
	conf_forget_network("ssid=Home\npsk=x1234567\n", "Home", out, sizeof(out));
	expect("forget the only network", out, "");
	conf_forget_network("ssid=Home\nfw=/x\n", "Home", out, sizeof(out));
	expect("forget when no psk follows keeps the next line", out, "fw=/x\n");

	if (conf_has_ssid(two, "Work") != 1 || conf_has_ssid(two, "Wor") != 0 || conf_has_ssid(two, "") != 0) {
		printf("FAIL conf_has_ssid\n"); failures++;
	} else
		printf("ok   conf_has_ssid\n");

	/* too-small output buffer must fail cleanly, never overflow */
	{
		char tiny[10];
		if (conf_set_network(two, "Work", "newpass99", tiny, sizeof(tiny)) != -1) { printf("FAIL tiny buffer\n"); failures++; }
		else printf("ok   output buffer too small is reported\n");
	}

	printf("%s\n", failures ? "TESTS FAILED" : "all tests passed");
	return failures ? 1 : 0;
}
