#include "device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <net/bpf.h>
#include <net/if.h>
#include <pthread.h>
#include <errno.h>

static void *
device_reader_thread (void *arg);

static struct {
	int fd;
	char *buffer;
	int buffer_size;
	int terminate;
	pthread_t thread;
	__device_interrupt_handler_t handler;
} g_device = {-1, NULL, 0, 0, 0, NULL};

int
device_init (const char *device_name, __device_interrupt_handler_t handler) {
	int index, flag, err;
	char dev[16];
	struct ifreq ifr;

	for (index = 0; index < 4; index++) {
		snprintf(dev, sizeof(dev), "/dev/bpf%d", index);
		if ((g_device.fd = open(dev, O_RDWR, 0)) != -1) {
			break;
		}
	}
	if (g_device.fd == -1) {
		perror("open");
		goto ERROR;
	}
	strcpy(ifr.ifr_name, device_name);
	if (ioctl(g_device.fd, BIOCSETIF, &ifr) == -1) {
		perror("ioctl [BIOCSETIF]");
		goto ERROR;
	}
	if (ioctl(g_device.fd, BIOCGBLEN, &g_device.buffer_size) == -1) {
		perror("ioctl [BIOCGBLEN]");
		goto ERROR;
	}
	if ((g_device.buffer = malloc(g_device.buffer_size)) == NULL) {
		perror("malloc");
		goto ERROR;
	}
	if (ioctl(g_device.fd, BIOCPROMISC, NULL) == -1) {
		perror("ioctl [BIOCPROMISC]");
		goto ERROR;
	}
	flag = 1;
	if (ioctl(g_device.fd, BIOCSSEESENT, &flag) == -1) {
		perror("ioctl [BIOCSSEESENT]");
		goto ERROR;
	}
	flag = 1;
	if (ioctl(g_device.fd, BIOCIMMEDIATE, &flag) == -1) {
		perror("ioctl [BIOCIMMEDIATE]");
		goto ERROR;
	}
	flag = 1;
	if (ioctl(g_device.fd, BIOCSHDRCMPLT, &flag) == -1) {
		perror("ioctl [BIOCSHDRCMPLT]");
		goto ERROR;
	}
	g_device.handler = handler;
	if ((err = pthread_create(&g_device.thread, NULL, device_reader_thread, NULL)) != 0) {
		fprintf(stderr, "pthread_create: error.\n");
		goto ERROR;
	}
	return  0;

ERROR:
	return -1;
}

void
device_cleanup (void) {
	if (g_device.thread) {
		g_device.terminate = 1;
		pthread_join(g_device.thread, NULL);
		g_device.thread = 0;
	}
	g_device.terminate = 0;
	if (g_device.fd != -1) {
		close(g_device.fd);
		g_device.fd = -1;
	}
	free(g_device.buffer);
	g_device.buffer = NULL;
	g_device.buffer_size = 0;
	g_device.handler = NULL;
}

static void *
device_reader_thread (void *arg) {
	ssize_t len = 0, bpf_frame;
	struct bpf_hdr *hdr;

	while (!g_device.terminate) {
		if (len <= 0) {
			len = read(g_device.fd, g_device.buffer, g_device.buffer_size);
			if (len == -1) {
				if (errno != EINTR) {
					perror("read");
					break;
				}
				continue;
			}
			hdr = (struct bpf_hdr *)g_device.buffer;
		} else {
			hdr = (struct bpf_hdr *)((caddr_t)hdr + BPF_WORDALIGN(hdr->bh_hdrlen + hdr->bh_caplen));
		}
		if (g_device.handler) {
			g_device.handler((uint8_t *)((caddr_t)hdr + hdr->bh_hdrlen), hdr->bh_caplen);
		}
		len -= BPF_WORDALIGN(hdr->bh_hdrlen + hdr->bh_caplen);
	}
	pthread_exit(NULL);
}

ssize_t
device_write (const uint8_t *buffer, size_t len) {
	if (!buffer) {
		return -1;
	}
	return write(g_device.fd, buffer, len);
}

ssize_t
device_writev (const struct iovec *iov, int iovcnt) {
	return writev(g_device.fd, iov, iovcnt);
}

#ifdef _DEVICE_UNIT_TEST
void
dummy_function (uint8_t *buf, ssize_t len) {
	printf("input: %ld\n", len);
}

int
main (int argc, char *argv[]) {
	if (device_init("en0", dummy_function) == -1) {
		goto ERROR;
	}
	sleep(10);
	device_cleanup();
	return  0;

ERROR:
	device_cleanup();
	return -1;
}
#endif