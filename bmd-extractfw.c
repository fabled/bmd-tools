#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXSIZE		2*1024*1024

#define array_size(x)	(sizeof(x) / sizeof(x[0]))

struct fwspec {
	const char *filename;
	uint8_t needle[4+16];
};

static const struct fwspec specs[] = {
	{
		"bmd-atemtvstudio.bin",
		{ 0x10, 0x27, 0x00, 0x00, 0x12, 0x01, 0x00, 0x02,
		  0xff, 0xff, 0x00, 0x40, 0xdb, 0x1e, 0x52, 0xbd,
		  0x00, 0x01, 0x01, 0x02 }
	}, {
		"bmd-h264prorecorder.bin",
		{ 0x10, 0x2c, 0x00, 0x00, 0x12, 0x01, 0x00, 0x02,
		  0xff, 0xff, 0x00, 0x40, 0xdb, 0x1e, 0x43, 0xbd,
		  0x00, 0x01, 0x01, 0x02 }
	}, {
		"bmd-h264prorecorder.bin",
		{ 0x10, 0x2c, 0x00, 0x00, 0x12, 0x01, 0x00, 0x02,
		  0xff, 0xff, 0x00, 0x40, 0xdb, 0x1e, 0x43, 0xbd,
		  0x00, 0x02, 0x01, 0x02 }
	}
};

struct hdr {
	uint8_t len, addr_high, addr_low, marker;
};

int main(int argc, char **argv)
{
	uint8_t *data, *dend, *fwbase, *fwend;
	struct hdr *h;
	ssize_t len, i;
	int ret = 0, fd;

	if (isatty(0)) {
		fprintf(stderr, "Usage: %s < BMDStreamingServer.exe\n", argv[0]);
		return 0;
	}

	data = malloc(MAXSIZE);
	if (data == NULL) {
		fprintf(stderr, "Out of memory\n");
		return 1;
	}

	len = read(0, data, MAXSIZE);
	dend = data + len;

	for (i = 0; i < array_size(specs); i++) {
		fwbase = memmem(data, len, specs[i].needle, sizeof(specs[i].needle));
		if (fwbase == NULL)
			continue;

		for (h = (struct hdr*) fwbase; h->marker == 0 && h < (struct hdr*) dend; )
			h = (struct hdr*) ((uint8_t*)h + h->len + 4 + 1);
		if (h >= (struct hdr*) dend)
			continue;

		fwend = (uint8_t*)h + 5;
		fprintf(stderr, "%s: @%08x, %ud bytes\n", specs[i].filename,
			(unsigned int)(fwbase - data), (unsigned int)(fwend - fwbase));

		fd = creat(specs[i].filename, 0666);
		if (fd < 0) {
			fprintf(stderr, "%s: failed to open: %s\n", specs[i].filename, strerror(errno));
			continue;
		}
		if (write(fd, fwbase, fwend - fwbase) != fwend-fwbase) {
			fprintf(stderr, "%s: write error\n", specs[i].filename);
			close(fd);
			unlink(specs[i].filename);
			ret = 2;
			continue;
		}
		close(fd);
	}

	return ret;
}

