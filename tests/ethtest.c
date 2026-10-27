/* ethtest - open /dev/net/iwi/0 like the network stack does and ask for the MAC address and frame size */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

int
main(void)
{
	unsigned char mac[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	int fd = open("/dev/net/iwi/0", O_RDWR), size = 0, r;

	if (fd < 0) { perror("open"); return 1; }
	r = ioctl(fd, 9999, mac, 6);
	printf("GETADDR rc=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n", r, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	r = ioctl(fd, 10005, &size, 4);
	printf("GETFRAMESIZE rc=%d size=%d\n", r, size);
	close(fd);
	return 0;
}
