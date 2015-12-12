/* BlackMagic Design tools - h264 stream over USB extractor
 * - ATEM TV Studio
 * - H264 Pro Encoder
 *
 * These devices seem to use Fujitsu MB86H56 for encoding HD H264 video.
 * Getting technical datasheet to that chip would make things a lot clearer.
 */

#include <math.h>
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <libusb.h>

#include "blackmagic.h"

/*
 * TODO and ideas:
 * - Get VR_SET_AUDIO_DELAY for remaining modes from USB traces
 * - Selecting capture target format - now it's "Native (Progressive)"
 * - Make the capture thread cancellable
 */

#define array_size(x) (sizeof(x) / sizeof(x[0]))

struct encoding_parameters {
	uint16_t	video_kbps, video_max_kbps, audio_kbps, audio_khz;
	uint8_t		h264_profile, h264_level, h264_bframes, h264_cabac, fps_divider;
	int8_t		input_source;
	char *		exec_program;
	int		respawn : 1;
	int		native_mode : 1;
};

static int do_syslog = 0;
static int loglevel = LOG_NOTICE;
static int firmware_fd = AT_FDCWD;
static int running = 1;
static volatile int num_workers = 0;
static struct encoding_parameters ep = {
	.video_kbps = 3000,
	.video_max_kbps = 3500,
	.h264_profile = FX2_H264_HIGH,
	.h264_level = 42,
	.h264_cabac = 1,
	.h264_bframes = 1,
	.audio_kbps = 256,
	.audio_khz = 48000,
	.fps_divider = 1,
	.input_source = -1,
	.native_mode = 0,
};

static const char *input_source_names[5] = {
	[INPUT_COMPONENT] = "component",
	[INPUT_HDMI] = "hdmi",
	[INPUT_SDI] = "sdi",
	[INPUT_COMPOSITE] = "composite",
	[INPUT_SVIDEO] = "s-video",
};

enum DISPLAY_MODE {
	DMODE_720x480i_29_97 = 0,
	DMODE_720x576i_25,
	DMODE_invalid,
	DMODE_720x480p_59_94,
	DMODE_720x576p_50,
	DMODE_1920x1080p_23_976,
	DMODE_1920x1080p_24,
	DMODE_1920x1080p_25,
	DMODE_1920x1080p_29_97,		/* 0x08 */
	DMODE_1920x1080p_30,
	DMODE_1920x1080i_25,
	DMODE_1920x1080i_29_97,
	DMODE_1920x1080i_30,
	DMODE_1920x1080p_50,
	DMODE_1920x1080p_59_94,
	DMODE_1920x1080p_60,
	DMODE_1280x720p_50,		/* 0x10 */
	DMODE_1280x720p_59_94,
	DMODE_1280x720p_60,
	DMODE_MAX
};

struct display_mode {
	const char *	description;
	int		width, height;
	int		fps_numerator, fps_denominator;

	uint8_t		interlaced : 1;
	uint8_t		program_fpga : 1;
	uint8_t		convert_to_1088 : 1;
	uint8_t		fx2_fps;
	uint8_t		audio_delay;
	uint16_t	ain_offset;
	uint16_t	r1000, r1404, r140a, r1430_l;
	uint16_t	r147x[4];
	uint16_t	r154x[11];
	struct display_mode	*native_mode;
};

static struct display_mode *display_modes[DMODE_MAX] = {
	[DMODE_720x480i_29_97] = &(struct display_mode){
		.description = "480i 29.97",
		.width = 720, .height = 486, .interlaced = 1, .program_fpga = 1,
		.fps_numerator = 30000, .fps_denominator = 1001, .fx2_fps = 0x4,
		.audio_delay = 0x27, .ain_offset = 0x0000,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a = 0x17ff, .r1430_l = 0xff,
		.r147x = { 0x10, 0x70, 0x70, 0x10 },
		.r154x = { 0x1050, 0x0002, 0x07ff, 0x035a, 0x020d, 0x008a, 0x002c, 0x07ff, 0x02d0, 0x01e8, 0x001e },
	},
	[DMODE_720x576i_25] = &(struct display_mode){
		.description = "576i 25",
		.width = 720, .height = 576, .interlaced = 1, .program_fpga = 1,
		.fps_numerator = 25, .fps_denominator = 1, .fx2_fps = 0x3,
		.audio_delay = 0x30, .ain_offset = 0x0000,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a = 0x17ff, .r1430_l = 0xff,
		.r147x = { 0x10, 0x70, 0x70, 0x10 },
		.r154x = { 0x1050, 0x0000, 0x07ff, 0x0360, 0x0271, 0x0090, 0x002e, 0x07ff, 0x02d0, 0x0240, 0x0019 },
	},
	/* DMODE_720x480p_59_94 */
	/* DMODE_720x576p_50 */
	[DMODE_1920x1080p_23_976] = &(struct display_mode){
		.description = "1080p 23.97",
		.width = 1920, .height = 1080,
		.fps_numerator = 24000, .fps_denominator = 1001, .fx2_fps = 0x1,
		.ain_offset = 0x0177,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a = 0x17ff, .r1430_l = 0xff,
		.r147x = { 0x10, 0x70, 0x70, 0x10 },
		.r154x = { 0x0000, 0x0001, 0x07ff, 0x0abe, 0x0465, 0x033e, 0x0015, 0x07ff, 0x0780, 0x0438, 0x0018 },
	},
	[DMODE_1920x1080p_24] = &(struct display_mode){
		.description = "1080p 24",
		.width = 1920, .height = 1080,
		.fps_numerator = 24, .fps_denominator = 1, .fx2_fps = 0x2,
		.ain_offset = 0x0000,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a = 0x17ff, .r1430_l = 0xff,
		.r147x = { 0x10, 0x70, 0x70, 0x10 },
		.r154x = { 0x0000, 0x0001, 0x07ff, 0x0abe, 0x0465, 0x033e, 0x0015, 0x07ff, 0x0780, 0x0438, 0x0018 },
	},
	[DMODE_1920x1080p_25] = &(struct display_mode){
		.description = "1080p 25",
		.width = 1920, .height = 1080,
		.fps_numerator = 25, .fps_denominator = 1, .fx2_fps = 0x3,
		.ain_offset = 0x0708,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a = 0x17ff, .r1430_l = 0xff,
		.r147x = { 0x26, 0x7d, 0x56, 0x07 },
		.r154x = { 0x0000, 0x0001, 0x07ff, 0x0abd, 0x0465, 0x00c3, 0x0015, 0x07ff, 0x0780, 0x0438, 0x0019 },
	},
	/* DMODE_1920x1080p_29_97 */
	[DMODE_1920x1080p_30] = &(struct display_mode){
		.description = "1080p 30",
		.width = 1920, .height = 1080,
		.fps_numerator = 30, .fps_denominator = 1, .fx2_fps = 0x5,
		.ain_offset = 0x0a8c,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a = 0x17ff, .r1430_l = 0xff,
		.r147x = { 0x26, 0x7d, 0x56, 0x07 },
		.r154x = { 0x0000, 0x0001, 0x07ff, 0x0897, 0x0465, 0x00c5, 0x0015, 0x07ff, 0x0780, 0x0438, 0x001e },
	},
	[DMODE_1920x1080i_25] = &(struct display_mode){
		.description = "1080i 50",
		.width = 1920, .height = 1080, .interlaced = 0, .convert_to_1088 = 1,
		.fps_numerator = 25, .fps_denominator = 1, .fx2_fps = 0x3,
		.ain_offset = 0x0000,
		.r1000 = 0x0200, .r1404 = 0x0041, .r140a = 0x1701, .r1430_l = 0xff,
		.r147x = { 0x26, 0x7d, 0x56, 0x07 },
		.r154x = { 0x0000, 0x0034, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x000e, 0x0780, 0x0438, 0x0000 },
		.native_mode = &(struct display_mode){
			.description = "1080i 50",
			.width = 1920, .height = 1080, .interlaced = 1, .convert_to_1088 = 0,
			.fps_numerator = 25, .fps_denominator = 1, .fx2_fps = 0x3,
			.ain_offset = 0x0000,
			.r1000 = 0x0200, .r1404 = 0x0071, .r140a = 0x1001, .r1430_l = 0x02,
			.r147x = { 0x26, 0x7d, 0x56, 0x07 },
			.r154x = { 0x0100, 0x0001, 0x07ff, 0x0a50, 0x0465, 0x02d0, 0x0015, 0x07ff, 0x0780, 0x0438, 0x0000 },
		}
	},
	[DMODE_1920x1080i_29_97] = &(struct display_mode){
		.description = "1080i 29.97",
		.width = 1920, .height = 1080, .interlaced = 1, .convert_to_1088 = 1,
		.fps_numerator = 30000, .fps_denominator = 1001, .fx2_fps = 0x4,
		.ain_offset = 0x0000,
		.r1000 = 0x0200, .r1404 = 0x0071, .r140a = 0x1700, .r1430_l = 0xff,
		.r147x = { 0x26, 0x7d, 0x56, 0x07 },
		.r154x = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x000e, 0x0000, 0x0400, 0x0000 },
	},
	[DMODE_1920x1080i_30] = &(struct display_mode){
		.description = "1080i 30",
		.width = 1920, .height = 1080, .interlaced = 1, .convert_to_1088 = 1, .program_fpga = 1,
		.fps_numerator = 30, .fps_denominator = 1, .fx2_fps = 0x5,
		.ain_offset = 0x0000,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a = 0x17ff, .r1430_l = 0xff,
		.r147x = { 0x10, 0x70, 0x70, 0x10 },
		.r154x = { 0x0000, 0x0001, 0x07ff, 0x0898, 0x0465, 0x0118, 0x0015, 0x07ff, 0x0780, 0x0438, 0x001e },
	},
	[DMODE_1920x1080p_50] = &(struct display_mode){
		.description = "1080p 50",
		.width = 1920, .height = 1080, .interlaced = 0,
		.fps_numerator = 50, .fps_denominator = 1, .fx2_fps = 0x6,
		.ain_offset = 0x05a0,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a = 0x17ff, .r1430_l = 0xff,
		.r147x = { 0x10, 0x70, 0x70, 0x10 },
		.r154x = { 0x0000, 0x0001, 0x07ff, 0x0a50, 0x0465, 0x02d0, 0x0015, 0x07ff, 0x0780, 0x0438, 0x0032 },
	},
	[DMODE_1920x1080p_59_94] = &(struct display_mode){
		.description = "1080p 59.94",
		.width = 1920, .height = 1080, .interlaced = 0,
		.fps_numerator = 60000, .fps_denominator = 1001, .fx2_fps = 0x7,
		.ain_offset = 0x0000,
		.r1000 = 0x0200, .r1404 = 0x0071, .r140a = 0x151e, .r1430_l = 0x02,
		.r147x = { 0x10, 0x70, 0x70, 0x10 },
		.r154x = { 0x0000, 0x0001, 0x07ff, 0x0898, 0x0465, 0x0118, 0x0015, 0x07ff, 0x0780, 0x0438, 0x0000 },
	},
	[DMODE_1920x1080p_60] = &(struct display_mode){
		.description = "1080p 60",
		.width = 1920, .height = 1080, .interlaced = 0,
		.fps_numerator = 60, .fps_denominator = 1, .fx2_fps = 8,
		.ain_offset = 0x079e,
		.r1000 = 0x0200, .r1404 = 0x0071, .r140a = 0x151e, .r1430_l = 0x02,
		.r147x = { 0x26, 0x7d, 0x56, 0x07 },
		.r154x = { 0x0000, 0x0001, 0x07ff, 0x0897, 0x0465, 0x00c5, 0x0015, 0x07ff, 0x0780, 0x0438, 0x0000 },
	},
	[DMODE_1280x720p_50] = &(struct display_mode){
		.description = "720p 50",
		.width = 1280, .height = 720, .interlaced = 0,
		.fps_numerator = 50, .fps_denominator = 1, .fx2_fps = 0x6,
		.audio_delay = 0x05, .ain_offset = 0x0384,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a = 0x17ff, .r1430_l = 0xff,
		.r147x = { 0x10, 0x70, 0x70, 0x10 },
		.r154x = { 0x0000, 0x0001, 0x07ff, 0x07bb, 0x02ee, 0x0107, 0x001a, 0x07ff, 0x0500, 0x02d0, 0x0032 },
	},
	[DMODE_1280x720p_59_94] = &(struct display_mode){
		.description = "720p 59.94",
		.width = 1280, .height = 720, .interlaced = 0,
		.fps_numerator = 60000, .fps_denominator = 1001, .fx2_fps = 0x7,
		.audio_delay = 0x07, .ain_offset = 0x0384,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a = 0x17ff, .r1430_l = 0xff,
		.r147x = { 0x10, 0x70, 0x70, 0x10 },
		.r154x = { 0x0000, 0x0001, 0x07ff, 0x07bb, 0x02ee, 0x0107, 0x001a, 0x07ff, 0x0500, 0x02d0, 0x003c },
	},
	[DMODE_1280x720p_60] = &(struct display_mode){
		.description = "720p 60",
		.width = 1280, .height = 720,
		.fps_numerator = 60, .fps_denominator = 1, .fx2_fps = 0x8,
		.audio_delay = 0x06, .ain_offset = 0x02ee,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a = 0x17ff, .r1430_l = 0xff,
		.r147x = { 0x10, 0x70, 0x70, 0x10 },
		.r154x = { 0x0000, 0x0001, 0x07ff, 0x0671, 0x02ee, 0x010b, 0x001a, 0x07ff, 0x0500, 0x02d0, 0x003c },
	},
};

enum {
	FX2Status_Unknown = 0,
	FX2Status_NotPowered,
	FX2Status_UpdatingFirmware,
	FX2Status_Programming,
	FX2Status_Booting,
	FX2Status_Idle,
	FX2Status_PreparingEncode,
	FX2Status_Encoding,
	FX2Status_PreparingNullOutput,
	FX2Status_NullOutput,
	FX2Status_PreparingStop,
	FX2Status_Stopped,
	FX2Status_InvalidFPGA
};

static const char* FX2Status_to_String(int s)
{
	switch (s) {
	case FX2Status_Unknown:		return "Unknown";
	case FX2Status_NotPowered:	return "Not powered";
	case FX2Status_UpdatingFirmware: return "Updating firmware";
	case FX2Status_Programming:	return "Programming H56";
	case FX2Status_Booting:		return "Booting H56";
	case FX2Status_Idle:		return "Idle";
	case FX2Status_PreparingEncode:	return "Preparing for Encode";
	case FX2Status_Encoding:	return "Encoding";
	case FX2Status_PreparingNullOutput: return "Preparing for Null output";
	case FX2Status_NullOutput:	return "Null output";
	case FX2Status_PreparingStop:	return "Preparing for Stop";
	case FX2Status_Stopped:		return "Stopped";
	case FX2Status_InvalidFPGA:	return "FPGA Firmware Invalid";
	default:			return "Bad Status";
	}
}

static int input_mode_to_display_mode(uint8_t mode)
{
	if (mode & 0x80)
		mode &= 0x9f;
	else
		mode &= 0x60;

	switch (mode) {
	case 0x00: return DMODE_720x480i_29_97;
	case 0x20: return DMODE_720x576i_25;
	case 0x40: return DMODE_720x480p_59_94;
	case 0x60: return DMODE_720x576p_50;
	case 0x80: return DMODE_1920x1080i_30;
	case 0x81: return DMODE_1920x1080i_29_97;
	case 0x82: return DMODE_1920x1080i_25;
	case 0x83: return DMODE_1920x1080p_25;
	case 0x84: case 0x86: return DMODE_1920x1080p_24;
	case 0x85: case 0x87: return DMODE_1920x1080p_23_976;
	case 0x88: return DMODE_1280x720p_60;
	case 0x89: return DMODE_1280x720p_59_94;
	case 0x8a: return DMODE_1280x720p_50;
	case 0x8e: return DMODE_1920x1080p_30;
	case 0x8f: return DMODE_1920x1080p_29_97;
	case 0x90: return DMODE_1920x1080p_60;
	case 0x91: return DMODE_1920x1080p_59_94;
	case 0x92: return DMODE_1920x1080p_50;
	default: return DMODE_invalid;
	}
}

static void hexdump(char *buf, size_t buflen, const char *ptr, size_t ptrlen)
{
	int i;

	for (i = 0; i < ptrlen && (i+1)*2+1 <= buflen; i++)
		snprintf(&buf[i*2], 3, "%02x", (unsigned char) ptr[i]);
}

static void dlog(int prio, const char *format, ...)
{
	va_list va;

	if (prio > loglevel) return;

	va_start(va, format);
	if (do_syslog)
		vsyslog(prio, format, va);
	else {
		flockfile(stderr);
		vfprintf(stderr, format, va);
		fputc('\n', stderr);
		funlockfile(stderr);
	}
	va_end(va);
}

struct firmware {
	uint32_t	size;
	uint16_t	device_id;
	uint8_t		data[];
};

static struct firmware *firmwares[2];

static struct firmware *load_firmware(const char *filename, uint16_t device_id)
{
	struct firmware *fw;
	struct stat st;
	int r, fd;

	r = fstatat(firmware_fd, filename, &st, 0);
	if (r != 0) {
		dlog(LOG_ERR, "%s: failed to load firmware to memory", filename);
		return NULL;
	}

	fw = malloc(sizeof(struct firmware) + st.st_size);
	if (fw == NULL)
		return NULL;

	fw->size = st.st_size;
	fw->device_id = device_id;

	fd = openat(firmware_fd, filename, O_RDONLY);
	if (fd < 0)
		goto error;
	if (read(fd, fw->data, fw->size) != fw->size)
		goto error;
	close(fd);
	return fw;
error:
	close(fd);
	free(fw);
	return NULL;
}

struct mpeg_parser_buffer {
	int output_fd;
	int oldlen;
	unsigned char olddata[0xbc];
	unsigned char data[16*1024];
};

static int mpegparser_parse(struct mpeg_parser_buffer *pb, int newlen)
{
	struct iovec iov[64];
	unsigned char *buf = &pb->data[-pb->oldlen];
	int i = 0, len = newlen + pb->oldlen, r = 0, ioc = 0, nomerge = 1;

	while (i + 0xbc <= len) {
		if (memcmp(&buf[i], "\x00\x00\x00\x00", 4) == 0) goto skip_block;
		if (buf[i] != 0x47) {
			while (buf[i] != 0x47 && i < len)
				i++;
			goto skip;
		}
		if (buf[i+1] == 0x1f && buf[i+2] == 0xff) goto skip_block;
		if (pb->output_fd < 0 || r) {
		skip_block:
			i += 0xbc;
		skip:
			nomerge = 1;
			continue;
		}

		if (nomerge) {
			nomerge = 0;
			iov[ioc].iov_base = &buf[i];
			iov[ioc].iov_len = 0xbc;
			if (++ioc >= array_size(iov)) {
				if (writev(pb->output_fd, iov, ioc) < 0) {
					dlog(LOG_NOTICE, "error writing MPEG TS: %s",
						strerror(errno));
					if (errno == EPIPE) r = -1;
				}
				ioc = 0;
				nomerge = 1;
			}
		} else {
			iov[ioc-1].iov_len += 0xbc;
		}
		i += 0xbc;
	}

	if (ioc && writev(pb->output_fd, iov, ioc) < 0) {
		dlog(LOG_NOTICE, "error writing MPEG TS: %s",
			strerror(errno));
		if (errno == EPIPE) r = -1;
	}

	pb->oldlen = len - i;
	memcpy(&pb->data[-pb->oldlen], &buf[i], pb->oldlen);
	return r;
}

struct blackmagic_device {
	char name[64];
	pthread_t device_thread, mpegts_thread;
	volatile int running;
	int status;
	int fxstatus;
	int recognized : 1;
	int encode_sent : 1;
	int display_mode_changed : 1;

	int current_display_mode;
	struct display_mode *current_mode;

	struct libusb_device_descriptor desc;
	libusb_device *usbdev;
	libusb_device_handle *usbdev_handle;

	uint8_t mac[6];

	uint8_t message_buffer[1024];
	struct mpeg_parser_buffer mpegparser;
};

static void reapchildren(int sig)
{
	int status;

	while (waitpid(-1, &status, WNOHANG) == 0 || errno == EINTR);
}

static void dostop(int sig)
{
	running = 0;
}

static void bmd_set_input_source(struct blackmagic_device *bmd, uint8_t mode)
{
	int r;
	if (bmd->status != LIBUSB_SUCCESS)
		return;
	dlog(LOG_NOTICE, "%s: switching input source to %s (%d)",
		bmd->name, input_source_names[mode], mode);
	r = libusb_control_transfer(
		bmd->usbdev_handle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
		VR_SET_INPUT_SOURCE, 0x0000, 0, &mode, 1, 1000);
	if (r < 0)
		bmd->status = r;
}

static void bmd_read_register(struct blackmagic_device *bmd, uint8_t reg, uint8_t *value)
{
	int r;
	if (bmd->status != LIBUSB_SUCCESS)
		return;
	r = libusb_control_transfer(
		bmd->usbdev_handle, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
		VR_READ_REGISTER, 0x0000, reg << 8, value, 1, 1000);
	if (r < 0)
		bmd->status = r;
}

static void bmd_load_firmware(struct blackmagic_device *bmd, uint16_t address, uint8_t *data, size_t len)
{
	int r;
	if (bmd->status != LIBUSB_SUCCESS)
		return;
	r = libusb_control_transfer(
		bmd->usbdev_handle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
		CYPRESS_VR_FIRMWARE_LOAD, address, 0, data, len, 1000);
	if (r < 0)
		bmd->status = r;
}

static uint16_t bmd_fujitsu_read(struct blackmagic_device *bmd, uint32_t reg)
{
	uint8_t value[2];
	int r;

	if (bmd->status != LIBUSB_SUCCESS)
		return 0;

	r = libusb_control_transfer(
		bmd->usbdev_handle, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
		VR_FUJITSU_READ, reg & 0xffff, (reg >> 16) & 0xff,
		value, sizeof(value), 1000);
	if (r != 2)
		return 0;

	return (value[0] << 8) | value[1];
}

static void bmd_fujitsu_write(struct blackmagic_device *bmd, uint32_t reg, uint16_t value)
{
	uint8_t msg[5];
	int r;

	if (bmd->status != LIBUSB_SUCCESS)
		return;

	msg[0] = reg >> 16;
	msg[1] = reg >> 8;
	msg[2] = reg;
	msg[3] = value >> 8;
	msg[4] = value;

	if (loglevel >= LOG_DEBUG) {
		uint16_t oldvalue = bmd_fujitsu_read(bmd, reg);
		if (value != oldvalue)
			dlog(LOG_DEBUG, "%s: fujitsu_write @%06x %04x != %04x",
				bmd->name, reg, value, oldvalue);
	}

	r = libusb_control_transfer(
		bmd->usbdev_handle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,  
		VR_FUJITSU_WRITE, 0, 0, msg, 5, 1000);
	if (r < 0)
		bmd->status = r;
}

static int bmd_upload_firmware(struct blackmagic_device *bmd, struct firmware *fw)
{
	struct _fwhdr { uint8_t len, addr_high, addr_low, marker, data[]; };
	struct _fwhdr *fwhdr;
	uint16_t addr;
	int i, pos = 0;

	bmd_load_firmware(bmd, 0xe600, "\x01", 1);
	while (pos < fw->size && bmd->status == LIBUSB_SUCCESS) {
		fwhdr = (struct _fwhdr *) &fw->data[pos];
		if (fwhdr->marker != 0)
			break;
		addr = (fwhdr->addr_high << 8) + fwhdr->addr_low;
		bmd_load_firmware(bmd, addr, fwhdr->data, fwhdr->len);
		pos += fwhdr->len + 5;
	}
	bmd_load_firmware(bmd, 0xe600, "\x00", 1);

	return bmd->status == LIBUSB_SUCCESS;
}

static int bmd_start_exec_program(struct blackmagic_device *bmd, char *exec_program)
{
	char tmp[1024];
	char *envp[16];
	char *argv[] = { exec_program, 0 };
	int r, i, p, pipefd[2];
	posix_spawn_file_actions_t fa;

	if (!exec_program) {
		bmd->mpegparser.output_fd = STDOUT_FILENO;
		return 1;
	}

	dlog(LOG_DEBUG, "%s: launching exec program: %s", bmd->name, ep.exec_program);

	if (pipe(pipefd) < 0)
		return 0;

	i = p = 0;
	if (bmd->desc.idProduct != USB_PID_BMD_H264_PRO_RECORDER) {
		envp[i++] = &tmp[p];
		p += snprintf(&tmp[p], sizeof(tmp)-p, "BMD_MAC=%02x%02x%02x%02x%02x%02x",
			bmd->mac[0], bmd->mac[1], bmd->mac[2], bmd->mac[3], bmd->mac[4], bmd->mac[5]) + 1;
	}
	envp[i++] = &tmp[p];
	p += snprintf(&tmp[p], sizeof(tmp)-p, "BMD_STREAM_WIDTH=%d", bmd->current_mode->width) + 1;
	envp[i++] = &tmp[p];
	p += snprintf(&tmp[p], sizeof(tmp)-p, "BMD_STREAM_HEIGHT=%d", bmd->current_mode->height) + 1;
	envp[i] = 0;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pipefd[0], STDIN_FILENO);
	posix_spawn_file_actions_addclose(&fa, pipefd[1]);
	r = posix_spawnp(NULL, argv[0], &fa, NULL, argv, envp);
	posix_spawn_file_actions_destroy(&fa);

	close(pipefd[0]);
	if (r != 0) {
		close(pipefd[1]);
		return 0;
	}

	bmd->mpegparser.output_fd = pipefd[1];
	fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
	return 1;
}

static void bmd_kill_exec_program(struct blackmagic_device *bmd)
{
	if (ep.exec_program && bmd->mpegparser.output_fd >= 0) {
		dlog(LOG_DEBUG, "%s: closing output stream", bmd->name);
		close(bmd->mpegparser.output_fd);
	}
	bmd->mpegparser.output_fd = -1;
}


static void *bmd_pump_mpegts(void *ctx)
{
	struct blackmagic_device *bmd = ctx;
	int actual_length, r;

	do {
		r = libusb_bulk_transfer(
			bmd->usbdev_handle, 0x86,
			bmd->mpegparser.data, sizeof(bmd->mpegparser.data),
			&actual_length, 5000);
		if (r != LIBUSB_SUCCESS && r != LIBUSB_ERROR_TIMEOUT)
			break;
		if (r == LIBUSB_ERROR_TIMEOUT)
			dlog(LOG_INFO, "%s: mpeg-ts pump: timeout reading data, retrying!", bmd->name);

		if (mpegparser_parse(&bmd->mpegparser, actual_length) < 0) {
			if (ep.exec_program) {
				if (ep.respawn) {
					bmd_kill_exec_program(bmd);
					bmd_start_exec_program(bmd, ep.exec_program);
				} else {
					bmd->running = 0;
				}
			} else
				running = 0;
		}
	} while (running && bmd->running);

	dlog(LOG_DEBUG, "%s: mpeg-ts pump exiting: %s", bmd->name, libusb_error_name(r));

	return NULL;
}

static int bmd_recognize_device(struct blackmagic_device *bmd)
{
	int i;

	bmd->recognized = 1;

	/* Pro Recorder does not have a NIC */
	if (bmd->desc.idProduct == USB_PID_BMD_H264_PRO_RECORDER)
		return 1;

	/* ATEM TV Studio register layout:
	 * 0x84..0x87	firmware version, timestamp or checksum
	 *	4.1.2 -> 71 70 86 c6
	 *	4.2.1 -> 7a 01 f1 34
	 * 0x88..0x8d	mac address
	 * 0xa0..0xa3	ipv4 address
	 * 0xa4..0xa7	ipv4 netmask
	 * 0xa8..0xab	ipv4 gateway
	 */

	for (i = 0; i < 6; i++)
		bmd_read_register(bmd, 0x88 + i, &bmd->mac[i]);

	dlog(LOG_NOTICE, "%s: MAC address %02x:%02x:%02x:%02x:%02x:%02x",
		bmd->name,
		bmd->mac[0], bmd->mac[1], bmd->mac[2],
		bmd->mac[3], bmd->mac[4], bmd->mac[5]);

	return bmd->status == LIBUSB_SUCCESS;
}

static int bmd_configure_encoder(struct blackmagic_device *bmd, struct encoding_parameters *ep)
{
	uint8_t fpga_command_1[1] = { 0x20 };
	uint8_t fpga_command_2[1] = { 0x40 };
	struct display_mode *current_mode = bmd->current_mode;
	uint32_t total_bandwidth;
	float fps;
	int r;

	fps = (float)current_mode->fps_numerator / current_mode->fps_denominator;

	/* apparently approximate of total bandwidth required */
	total_bandwidth  = 85226;
	total_bandwidth += (ep->audio_kbps * 1000.0 * 1024 / (8 * ep->audio_khz) + 14) / 148 * 1504.0 * ep->audio_khz / 1024;
	total_bandwidth += 48128.0 * fps / ((fps == 25 || fps == 50) ? 12 : 15);
	total_bandwidth += 1.021739130434783 * (ceil(1464*fps) + ceil(152*fps) + (ep->video_max_kbps + 1000) * 1000);

	r = libusb_control_transfer(
		bmd->usbdev_handle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
		current_mode->program_fpga ? VR_SEND_FPGA_COMMAND : VR_CLEAR_FPGA_COMMAND, 0, 0,
		fpga_command_1, sizeof(fpga_command_1), 1000);
	if (r < 0) {
		bmd->status = r;
		return 0;
	}
	r = libusb_control_transfer(
		bmd->usbdev_handle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
		VR_CLEAR_FPGA_COMMAND, 0, 0,
		fpga_command_2, sizeof(fpga_command_2), 1000);
	if (r < 0) {
		bmd->status = r;
		return 0;
	}

	r = libusb_control_transfer(
		bmd->usbdev_handle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,  
		VR_SET_AUDIO_DELAY, 0, 0, &current_mode->audio_delay, 1, 5000);
	if (r < 0) {
		bmd->status = r;
		return 0;
	}

	/* Group 1 - likely muxing related */
	bmd_fujitsu_write(bmd, 0x0800ea, 0x0a0c);
	bmd_fujitsu_write(bmd, 0x0800ec, 0x000d);
	bmd_fujitsu_write(bmd, 0x0800ee, 0x0000);
	bmd_fujitsu_write(bmd, 0x0800f0, 0x0504);
	bmd_fujitsu_write(bmd, 0x0800f2, 0x4844);
	bmd_fujitsu_write(bmd, 0x0800f4, 0x4d56);
	bmd_fujitsu_write(bmd, 0x0800f6, 0x8804);
	bmd_fujitsu_write(bmd, 0x0800f8, 0x0fff);
	bmd_fujitsu_write(bmd, 0x0800fa, 0xfcfc);
	bmd_fujitsu_write(bmd, 0x080100, 0x6308);
	bmd_fujitsu_write(bmd, 0x080102, 0xc000 | ((total_bandwidth/400) >> 8));
	bmd_fujitsu_write(bmd, 0x080104, 0x00ff | ((total_bandwidth/400) << 8));
	bmd_fujitsu_write(bmd, 0x080106, 0xffff);
	bmd_fujitsu_write(bmd, 0x080108, 0xffff);
	bmd_fujitsu_write(bmd, 0x080110, 0x1bf0);
	bmd_fujitsu_write(bmd, 0x080112, 0x11f0);
	bmd_fujitsu_write(bmd, 0x080114, 0x0302);
	bmd_fujitsu_write(bmd, 0x080116, 0x0102 | (current_mode->fx2_fps << 3));
	bmd_fujitsu_write(bmd, 0x080118, 0x0ff1); //(audiomode_related_fixed_var << 8) | 0xf1
	bmd_fujitsu_write(bmd, 0x08011a, 0x00f0);
	bmd_fujitsu_write(bmd, 0x08011c, 0x0000);

	/* Group 2 - MPEG TS muxer */
	bmd_fujitsu_write(bmd, 0x001000, current_mode->r1000);
	bmd_fujitsu_write(bmd, 0x001002, 0x8480);
	bmd_fujitsu_write(bmd, 0x001004, 0x0002);
	bmd_fujitsu_write(bmd, 0x001006, total_bandwidth / 1000);
	bmd_fujitsu_write(bmd, 0x001008, 0x0000);
	bmd_fujitsu_write(bmd, 0x00100c, 0x0000);
	bmd_fujitsu_write(bmd, 0x00100e, 0x0000);
	bmd_fujitsu_write(bmd, 0x001010, 0x0000);
	bmd_fujitsu_write(bmd, 0x001012, 0x0000);
	bmd_fujitsu_write(bmd, 0x001014, 0x0000);
	bmd_fujitsu_write(bmd, 0x001016, 0x1011);	// Video PID
	bmd_fujitsu_write(bmd, 0x001018, 0x1100);	// Audio PID
	bmd_fujitsu_write(bmd, 0x00101a, 0x0100);	// Program Map Table PID
	bmd_fujitsu_write(bmd, 0x00101c, 0x001f);	// DVB SIT PID
	bmd_fujitsu_write(bmd, 0x00101e, 0x1001);	// Program clock PID
	bmd_fujitsu_write(bmd, 0x001020, 0x00e0);	// Video PES stream ID
	bmd_fujitsu_write(bmd, 0x001022, 0x00c0);	// Audio PES stream ID
	bmd_fujitsu_write(bmd, 0x001146, 0x0101);
	bmd_fujitsu_write(bmd, 0x001148, 0x0100);

	/* Group 3 - H264 encoder, video source tuning */
	bmd_fujitsu_write(bmd, 0x001404, current_mode->r1404);
	bmd_fujitsu_write(bmd, 0x001406, ep->video_max_kbps + 1000);
	bmd_fujitsu_write(bmd, 0x001408, ep->video_kbps);
	bmd_fujitsu_write(bmd, 0x00140a, current_mode->r140a);
	bmd_fujitsu_write(bmd, 0x00140c, ep->h264_cabac ? 0x0000 : 0x0100);
	bmd_fujitsu_write(bmd, 0x00140e, 0xd400 | ((current_mode->fps_denominator == 1) ? 0x0001 : 0x0000));
	bmd_fujitsu_write(bmd, 0x001418, 0x0001);
	bmd_fujitsu_write(bmd, 0x001420, 0x0000);
	bmd_fujitsu_write(bmd, 0x001422, ep->video_max_kbps);
	/* Register 0x1430 lower byte is related to INPUT MODE/TARGET MODE specific.
	 *  affects directly the output stream resolution, possibly TS mode bits. */
	bmd_fujitsu_write(bmd, 0x001430, current_mode->r1430_l | (ep->h264_bframes ? 0x0000 : 0x0100));
	bmd_fujitsu_write(bmd, 0x001470, current_mode->r147x[0]);
	bmd_fujitsu_write(bmd, 0x001472, current_mode->r147x[1]);
	bmd_fujitsu_write(bmd, 0x001474, current_mode->r147x[2]);
	bmd_fujitsu_write(bmd, 0x001476, current_mode->r147x[3]);
	bmd_fujitsu_write(bmd, 0x001478, 0x0000);
	bmd_fujitsu_write(bmd, 0x00147a, 0x0000);
	bmd_fujitsu_write(bmd, 0x00147c, 0x0000);
	bmd_fujitsu_write(bmd, 0x00147e, 0x0000);

	/* INPUT MODE based constants likely tuning for sync or similar,
	 * some of these seem to get ignored (and initialized to random
	 * value by the BMD drivers). */
	bmd_fujitsu_write(bmd, 0x001540, current_mode->r154x[0]);
	bmd_fujitsu_write(bmd, 0x001542, current_mode->r154x[1]);
	bmd_fujitsu_write(bmd, 0x001544, current_mode->r154x[2]);
	bmd_fujitsu_write(bmd, 0x001546, current_mode->r154x[3]);
	bmd_fujitsu_write(bmd, 0x001548, current_mode->r154x[4]);
	bmd_fujitsu_write(bmd, 0x00154a, current_mode->r154x[5]);
	bmd_fujitsu_write(bmd, 0x00154c, current_mode->r154x[6]);
	bmd_fujitsu_write(bmd, 0x00154e, current_mode->r154x[7]);
	bmd_fujitsu_write(bmd, 0x001550, current_mode->r154x[8]);
	bmd_fujitsu_write(bmd, 0x001552, current_mode->r154x[9]);
	bmd_fujitsu_write(bmd, 0x001554, current_mode->r154x[10]);

	/* Group 4 - Audio encoder */
	switch (ep->audio_khz) {
	case 32000:
		bmd_fujitsu_write(bmd, 0x001802, 2);
		break;
	case 44100:
		bmd_fujitsu_write(bmd, 0x001802, 1);
		break;
	case 48000:
	default:
		bmd_fujitsu_write(bmd, 0x001802, 0);
		break;
	}
	bmd_fujitsu_write(bmd, 0x001804, ep->audio_kbps);
	bmd_fujitsu_write(bmd, 0x001806, 0x02c0);
	bmd_fujitsu_write(bmd, 0x001810, 0x0000);
	bmd_fujitsu_write(bmd, 0x001812, current_mode->ain_offset);
	if (0 /*audio_format == 5*/) {
		bmd_fujitsu_write(bmd, 0x001830, 0x0000);
	} else if (1 /*audio_format == 4 - AAC */) {
		bmd_fujitsu_write(bmd, 0x001850, 0x0033);
		bmd_fujitsu_write(bmd, 0x001852, 0x0200);
	}

	/* Group 5 - Scaler / H.264 encoder */
	if (0 /*target_mode*/) {
		// bit 0x8000 resolution converter enabled
		// bit 0x00ff conversion target, 0=no conversion, 4=NTSC, 5=PAL, 0xff=progressive
		bmd_fujitsu_write(bmd, 0x001520, 0x80ff);
		/*
		bmd_fujitsu_write(bmd, 0x001522, ep->mode->src_xoffs);	// src x offset
		bmd_fujitsu_write(bmd, 0x001524, ep->mode->src_yoffs);	// src y offset
		bmd_fujitsu_write(bmd, 0x001526, ep->mode->src_width);	// src width
		bmd_fujitsu_write(bmd, 0x001528, ep->mode->src_height);// src height (1080)
		bmd_fujitsu_write(bmd, 0x00152e, ep->mode->dst_width);	// dst width
		bmd_fujitsu_write(bmd, 0x001530, ep->mode->dst_height);// dst height (1088)
		*/
	} else if (current_mode->convert_to_1088) {
		/* Convert to height 1088 */
		bmd_fujitsu_write(bmd, 0x001520, 0x80ff);
		bmd_fujitsu_write(bmd, 0x001522, 0);			// src x offset
		bmd_fujitsu_write(bmd, 0x001524, 0);			// src y offset
		bmd_fujitsu_write(bmd, 0x001526, current_mode->width);	// src width
		bmd_fujitsu_write(bmd, 0x001528, current_mode->height);	// src height (1080)
		bmd_fujitsu_write(bmd, 0x00152e, current_mode->width);	// dst width
		bmd_fujitsu_write(bmd, 0x001530, 1088);			// dst height (1088)
	} else {
		bmd_fujitsu_write(bmd, 0x001520, 0);	// 0=conversion
		bmd_fujitsu_write(bmd, 0x001522, 0);	// src x offset
		bmd_fujitsu_write(bmd, 0x001524, 0);	// src y offset
		bmd_fujitsu_write(bmd, 0x001526, 0);	// src width
		bmd_fujitsu_write(bmd, 0x001528, 0);	// src height
		bmd_fujitsu_write(bmd, 0x00152e, 0);	// dst width
		bmd_fujitsu_write(bmd, 0x001530, 0);	// dst height
	}
	bmd_fujitsu_write(bmd, 0x0015a0, (ep->h264_profile << 14) | ep->h264_level);
	bmd_fujitsu_write(bmd, 0x0015a2, (current_mode->width + 15) >> 4);
	bmd_fujitsu_write(bmd, 0x0015a4, (current_mode->height + 15) >> 4);
	bmd_fujitsu_write(bmd, 0x0015a6, current_mode->fps_denominator); // divider
	bmd_fujitsu_write(bmd, 0x0015a8, 2*current_mode->fps_numerator/ep->fps_divider >> 16);
	bmd_fujitsu_write(bmd, 0x0015aa, 2*current_mode->fps_numerator/ep->fps_divider & 0xffff);
	bmd_fujitsu_write(bmd, 0x0015ac, 0x0001); // {1=HD,2=PAL,3=NTSC} depends on target resolution
	if (ep->fps_divider != 1) {
		/* Possibly input-output frame ratio */
		bmd_fujitsu_write(bmd, 0x0015b2, 0x8000 | ep->fps_divider);
		bmd_fujitsu_write(bmd, 0x0015b4, 1);
	} else {
		bmd_fujitsu_write(bmd, 0x0015b2, 0);
	}

	/* Group 6 - Enable */
	bmd_fujitsu_write(bmd, 0x001144, 0x3333);

	return bmd->status == LIBUSB_SUCCESS;
}

static void bmd_encoder_dump(struct blackmagic_device *bmd)
{
	uint32_t fps_numerator;

	fps_numerator = bmd_fujitsu_read(bmd, 0x0015a8);
	fps_numerator <<= 16;
	fps_numerator += bmd_fujitsu_read(bmd, 0x0015aa);
	fps_numerator /= 2;

	fprintf(stderr,
		"-------------------------------------------------------\n"
		"Detected mode: %02x\n"
		".fps_numerator = %d, .fps_denominator = %d, .fx2_fps = 0x%x,\n"
		".ain_offset = 0x%04x,\n"
		".r1000 = 0x%04x, .r1404 = 0x%04x, .r140a = 0x%04x, .r1430_l = 0x%02x,\n"
		".r147x = { 0x%02x, 0x%02x, 0x%02x, 0x%02x },\n"
		".r154x = { 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x },\n"
		"-------------------------------------------------------\n",
		bmd->current_display_mode,

		fps_numerator,
		bmd_fujitsu_read(bmd, 0x0015a6),
		(bmd_fujitsu_read(bmd, 0x080116) >> 3) & 0xf,

		bmd_fujitsu_read(bmd, 0x001812),

		bmd_fujitsu_read(bmd, 0x001000),
		bmd_fujitsu_read(bmd, 0x001404),
		bmd_fujitsu_read(bmd, 0x00140a),
		bmd_fujitsu_read(bmd, 0x001430) & 0xff,

		bmd_fujitsu_read(bmd, 0x001470),
		bmd_fujitsu_read(bmd, 0x001472),
		bmd_fujitsu_read(bmd, 0x001474),
		bmd_fujitsu_read(bmd, 0x001476),

		bmd_fujitsu_read(bmd, 0x001540),
		bmd_fujitsu_read(bmd, 0x001542),
		bmd_fujitsu_read(bmd, 0x001544),
		bmd_fujitsu_read(bmd, 0x001546),
		bmd_fujitsu_read(bmd, 0x001548),
		bmd_fujitsu_read(bmd, 0x00154a),
		bmd_fujitsu_read(bmd, 0x00154c),
		bmd_fujitsu_read(bmd, 0x00154e),
		bmd_fujitsu_read(bmd, 0x001550),
		bmd_fujitsu_read(bmd, 0x001552),
		bmd_fujitsu_read(bmd, 0x001554)
		);
}

static void bmd_encoder_start(struct blackmagic_device *bmd)
{
	const char *err;
	uint8_t status;
	int r;

	if (bmd->encode_sent || bmd->current_display_mode == DMODE_invalid)
		return;

	bmd->encode_sent = 1;

	if (!bmd->current_mode) {
		if (loglevel >= LOG_DEBUG)
			bmd_encoder_dump(bmd);
		return;
	}

	dlog(LOG_NOTICE, "%s: configuring and starting encoder", bmd->name);

	if (!bmd_configure_encoder(bmd, &ep)) {
		err = "configuring encoder";
		goto error;
	}

	if (!bmd_start_exec_program(bmd, ep.exec_program)) {
		err = "start exec program";
		goto error;
	}

	r = libusb_control_transfer(
		bmd->usbdev_handle, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
		VR_FUJITSU_START_ENCODING, 0x0004, 0,
		&status, sizeof(status), 2000);
	if (r < 0) {
		err = "start encoding";
		goto error;
	}
	return;
error:
	dlog(LOG_ERR, "%s: failed to %s", bmd->name, err);
}

static void bmd_encoder_stop(struct blackmagic_device *bmd)
{
	uint8_t status;
	uint8_t clear_fpga_command[1] = { 0x02 };
	uint8_t send_fpga_command[1] = { 0x80 };
	uint32_t fifo_level;
	int i, r;

	/* Stop recording */
	dlog(LOG_NOTICE, "%s: stopping encoder", bmd->name);
	bmd_kill_exec_program(bmd);

	r = libusb_control_transfer(
		bmd->usbdev_handle, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
		VR_FUJITSU_STOP_ENCODING, 0, 0, &status, sizeof(status), 1000);

	r = libusb_control_transfer(
		bmd->usbdev_handle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
		VR_CLEAR_FPGA_COMMAND, 0, 0,
		clear_fpga_command, sizeof(clear_fpga_command), 1000);

	r = libusb_control_transfer(
		bmd->usbdev_handle, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
		VR_GET_FIFO_LEVEL, 0, 0,
		(void*)&fifo_level, sizeof(fifo_level), 5000);

	for (i = 0; i < 67; i++) {
		r = libusb_control_transfer(
			bmd->usbdev_handle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
			VR_SEND_FPGA_COMMAND, 0, 0,
			send_fpga_command, sizeof(send_fpga_command), 1000);
	}
}

static void bmd_parse_message(struct blackmagic_device *bmd, const uint8_t *msg, int msg_len)
{
	int dm;

	switch (msg[0]) {
	case 0x01: /* Status update */
		dlog(LOG_DEBUG, "%s: FX2Status: %s (%d)", bmd->name, FX2Status_to_String(msg[5]), msg[5]);
		bmd->fxstatus = msg[5];
		break;
	case 0x05: /* Input connector */
		dm = input_mode_to_display_mode(msg[1]);
		dlog(LOG_DEBUG, "%s: DisplayMode: %02x", bmd->name, dm);
		if (dm != bmd->current_display_mode) {
			bmd->current_display_mode = dm;
			bmd->current_mode = display_modes[dm];
			if(ep.native_mode && bmd->current_mode->native_mode)
				bmd->current_mode = bmd->current_mode->native_mode;
			bmd->display_mode_changed = 1;
		}
		break;
	case 0x0d:
		dlog(LOG_ERR, "%s: H56 error; restarting device", bmd->name);
		break;
	case 0x0e: /* Timestamp update? */
		break;
	default:
		if (loglevel >= LOG_DEBUG) {
			char tmp[512];
			hexdump(tmp, sizeof(tmp), msg, msg_len);
			dlog(LOG_DEBUG, "%s: unknown message %s", bmd->name, tmp);
		}
		break;
	}
}

static void bmd_handle_messages(struct blackmagic_device *bmd, int force)
{
	int actual_length, r, i;

	do {
		r = libusb_bulk_transfer(
			bmd->usbdev_handle, 0x88,
			bmd->message_buffer, sizeof(bmd->message_buffer),
			&actual_length, 10000);
		if (r != LIBUSB_SUCCESS)
			break;

		/* The first 16-bits is the length of the full message */
		if (loglevel >= LOG_DEBUG) {
			char tmp[512];
			hexdump(tmp, sizeof(tmp), bmd->message_buffer, actual_length);
			dlog(LOG_DEBUG, "%s: ep8: %4d bytes: %s", bmd->name, actual_length, tmp);
		}

		/* Parse queued messages, especially during boot/first connect
		 * there can be lot of them, so process them allf irst. */
		for (i = 2; bmd->message_buffer[i] != 0 && i < actual_length;
		     i += bmd->message_buffer[i] + 1)
			bmd_parse_message(bmd, &bmd->message_buffer[i+1], bmd->message_buffer[i]);

		/* Act on status changes */
		switch (bmd->fxstatus) {
		case FX2Status_Idle:
			bmd->encode_sent = 0;
			if (!bmd->running)
				break;

			if (!bmd->recognized)
				bmd_recognize_device(bmd);

			if (bmd->current_mode)
				dlog(LOG_NOTICE, "%s: display mode: %s", bmd->name, bmd->current_mode->description);
			else if (bmd->current_display_mode == DMODE_invalid)
				dlog(LOG_NOTICE, "%s: no signal", bmd->name);
			else
				dlog(LOG_ERR, "%s: display mode: 0x%02x; not supported", bmd->name, bmd->current_display_mode);

			if (bmd->current_display_mode != DMODE_invalid)
				bmd_encoder_start(bmd);
			else if (bmd->display_mode_changed &&
				 bmd->desc.idProduct == USB_PID_BMD_H264_PRO_RECORDER &&
				 ep.input_source >= 0)
				bmd_set_input_source(bmd, ep.input_source);
			break;
		case FX2Status_Encoding:
			if (bmd->display_mode_changed || !bmd->encode_sent)
				bmd_encoder_stop(bmd);
			break;
		}
		bmd->display_mode_changed = 0;

	} while ((force && bmd->fxstatus != FX2Status_Idle) || (running && bmd->running));

	if (r != LIBUSB_SUCCESS) {
		dlog(LOG_INFO, "%s: message reader exiting: %s", bmd->name, libusb_error_name(r));
		bmd->status = r;
	}
}

static void *bmd_device_thread(void *ctx)
{
	struct blackmagic_device *bmd = ctx;
	int r, i;

	bmd->running = 1;
	bmd->current_display_mode = DMODE_invalid;
	bmd->mpegparser.output_fd = -1;

	/* Immediately after hotplug, the sysfs device nodes are not yet
	 * available. Unfortunately, libusb_open will disconnect mark device
	 * disconnected if the node does not exist... so just wait for
	 * (hopefully) long enough for the node to become available. */
	usleep(200 * 1000);

	r = libusb_open(bmd->usbdev, &bmd->usbdev_handle);
	if (r != LIBUSB_SUCCESS) {
		dlog(LOG_ERR, "%s: unable to open device: %s", bmd->name, libusb_error_name(r));
		goto exit;
	}

	r = libusb_set_configuration(bmd->usbdev_handle, 1);
	if (r != LIBUSB_SUCCESS) {
		dlog(LOG_ERR, "%s: failed to set configuration: %s", bmd->name, libusb_error_name(r));
		goto exit;
	}

	r = libusb_claim_interface(bmd->usbdev_handle, 0);
	if (r != LIBUSB_SUCCESS) {
		dlog(LOG_ERR, "%s: failed to claim interface: %s", bmd->name, libusb_error_name(r));
		goto exit;
	}

	if (bmd->desc.iManufacturer == 0) {
		const char *desc = "not available";

		dlog(LOG_INFO, "%s: firmware downloaded needed", bmd->name);
		for (i = 0; i < array_size(firmwares); i++) {
			if (firmwares[i]->device_id != bmd->desc.idProduct)
				continue;
			if (bmd_upload_firmware(bmd, firmwares[i]))
				desc = "downloaded succesfully";
			else
				desc = "failed to download";
			break;
		}
		dlog(LOG_NOTICE, "%s: firmware %s", bmd->name, desc);
	} else {
		if (bmd->desc.idProduct == USB_PID_BMD_H264_PRO_RECORDER &&
		    ep.input_source >= 0)
			bmd_set_input_source(bmd, ep.input_source);

		r = pthread_create(&bmd->mpegts_thread, NULL, bmd_pump_mpegts, bmd);
		if (r < 0)
			goto exit;

		r = libusb_control_transfer(
			bmd->usbdev_handle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
			VR_SEND_DEVICE_STATUS, 0, 0, 0, 0, 1000);
		if (r == LIBUSB_SUCCESS)
			bmd_handle_messages(bmd, 0);

		bmd->running = 0;
		if (bmd->mpegts_thread)
			pthread_join(bmd->mpegts_thread, NULL);

		if (bmd->fxstatus == FX2Status_Encoding && bmd->status == LIBUSB_SUCCESS) {
			bmd_encoder_stop(bmd);
			while (bmd->fxstatus != FX2Status_Idle && bmd->status == LIBUSB_SUCCESS)
				bmd_handle_messages(bmd, 1);
		}
	}

exit:
	dlog(LOG_INFO, "%s: closing device", bmd->name);
	bmd_kill_exec_program(bmd);
	libusb_close(bmd->usbdev_handle);
	libusb_unref_device(bmd->usbdev);
	free(bmd);

	__sync_sub_and_fetch(&num_workers, 1);

	return NULL;
}

static int handle_hotplug(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data)
{
	struct blackmagic_device *bmd;
	int r;

	if (!running)
		return 1;

	bmd = calloc(1, sizeof(struct blackmagic_device));
	if (bmd == NULL)
		return 0;

	(void) libusb_get_device_descriptor(dev, &bmd->desc);
	snprintf(bmd->name, sizeof(bmd->name), "[%d/%d %04x:%04x]",
		libusb_get_bus_number(dev), libusb_get_device_address(dev),
		bmd->desc.idVendor, bmd->desc.idProduct);
	bmd->usbdev = libusb_ref_device(dev);
	bmd->status = LIBUSB_SUCCESS;

	dlog(LOG_INFO, "%s: device connected", bmd->name);

	__sync_add_and_fetch(&num_workers, 1);

	r = pthread_create(&bmd->device_thread, NULL, bmd_device_thread, bmd);
	if (r != 0) {
		dlog(LOG_ERR, "%s: failed to create handler thread", bmd->name);
		libusb_unref_device(bmd->usbdev);
		free(bmd);
		return 0;
	}
	pthread_detach(bmd->device_thread);

	return 0;
}

static int usage(void)
{
	fprintf(stderr,
		"usage: bmd-streamer [OPTIONS]\n"
		"\n"
		"	-v,--verbose		Print more information\n"
		"	-k,--video-kbps		Set average video bitrate\n"
		"	-K,--video-max-kbps	Set maximum video bitrate\n"
		"	-a,--audio-kbps		Set audio bitrate\n"
		"	-P,--h264-profile	Set H.264 profile (high, main, baseline)\n"
		"	-L,--h264-level		Set H.264 level (40 = level 4.0, etc..)\n"
		"	-b,--h264-bframes	Allow using H.264 B-frames\n"
		"	-B,--h264-no-bframes	Disable using H.264 B-frames\n"
		"	-c,--h264-cabac		Allow using H.264 CABAC\n"
		"	-C,--h264-no-cabac	Disable using H.264 CABAC\n"
		"	-F,--fps-divider	Set framerate divider (input / stream)\n"
		"	-S,--input-source	Set input source (component, sdi, hdmi,\n"
		"				composite, s-video, or 0-4)\n"
		"	-f,--firmware-dir	Directory for firmare images\n"
		"	-x,--exec		Program to execute for each connected stream\n"
		"	-R,--respawn		Restart execute program if it exits\n"
		"	-s,--syslog		Log to syslog\n"
		"\n");
	return 1;
}

static int profile_string_to_int(const char *str)
{
	if (!strcmp(str, "high")) return FX2_H264_HIGH;
	if (!strcmp(str, "main")) return FX2_H264_MAIN;
	if (!strcmp(str, "baseline")) return FX2_H264_BASELINE;
	return -1;
}

int main(int argc, char **argv)
{
	static const struct option long_options[] = {
		{ "verbose",		no_argument, NULL, 'v' },
		{ "video-kbps",		required_argument, NULL, 'k' },
		{ "video-max-kbps",	required_argument, NULL, 'K' },
		{ "audio-kbps",		required_argument, NULL, 'a' },
		{ "h264-profile",	required_argument, NULL, 'P' },
		{ "h264-level",		required_argument, NULL, 'L' },
		{ "h264-bframes",	no_argument, NULL, 'b' },
		{ "h264-no-bframes",	no_argument, NULL, 'B' },
		{ "h264-cabac",		no_argument, NULL, 'c' },
		{ "h264-no-cabac",	no_argument, NULL, 'C' },
		{ "fps-divider",	required_argument, NULL, 'F' },
		{ "firmware-dir",	required_argument, NULL, 'f' },
		{ "input-source",	required_argument, NULL, 'S' },
		{ "exec",		required_argument, NULL, 'x' },
		{ "respawn",		no_argument, NULL, 'R' },
		{ "syslog",		no_argument, NULL, 's' },
		{ "native",		no_argument, NULL, 'n' },
		{ NULL }
	};
	static const char short_options[] = "vk:K:a:P:L:bcBCF:f:S:x:Rsn";

	libusb_context *ctx;
	libusb_hotplug_callback_handle cbhandle;
	const char *msg = NULL;
	int i, r, ec = 0, opt, optindex;

	signal(SIGCHLD, reapchildren);
	signal(SIGTERM, dostop);
	signal(SIGINT, dostop);
	signal(SIGPIPE, SIG_IGN);

	optindex = 0;
	while ((opt=getopt_long(argc, argv, short_options, long_options, &optindex)) > 0) {
		switch (opt) {
		case 's': do_syslog = 1; break;
		case 'x': ep.exec_program = optarg; break;
		case 'R': ep.respawn = 1; break;
		case 'f':
			if ((firmware_fd = open(optarg, O_DIRECTORY|O_RDONLY)) < 0) {
				perror("open");
				return usage();
			}
			break;
		case 'v': loglevel++; break;
		case 'k': ep.video_kbps = atoi(optarg); break;
		case 'K': ep.video_max_kbps = atoi(optarg); break;
		case 'a': ep.audio_kbps = atoi(optarg); break;
		case 'P':
			if ((ep.h264_profile = profile_string_to_int(optarg)) < 0)
				return usage();
			break;
		case 'L': ep.h264_level = atoi(optarg); break;
		case 'b': ep.h264_bframes = 1; break;
		case 'B': ep.h264_bframes = 0; break;
		case 'c': ep.h264_cabac = 1; break;
		case 'C': ep.h264_cabac = 0; break;
		case 'F': ep.fps_divider = atoi(optarg); break;
		case 'S':
			for (i = 0; i < array_size(input_source_names); i++)
				if (strcmp(optarg, input_source_names[i]) == 0)
					break;
			if (i >= array_size(input_source_names)) i = atoi(optarg);
			if (i >= array_size(input_source_names)) i = -1;
			ep.input_source = i;
			break;
		case 'n': ep.native_mode = 1; break;
		default:
			return usage();
		}
	}

	if (ep.fps_divider <= 0 || ep.fps_divider > 2) ep.fps_divider = 1;
	if (ep.video_max_kbps < ep.video_kbps) ep.video_max_kbps = ep.video_kbps + 100;

	firmwares[0] = load_firmware("bmd-atemtvstudio.bin", USB_PID_BMD_ATEM_TV_STUDIO);
	firmwares[1] = load_firmware("bmd-h264prorecorder.bin", USB_PID_BMD_H264_PRO_RECORDER);

	if (do_syslog)
		openlog("bmd-tools", 0, LOG_DAEMON);

	r = libusb_init(&ctx);
	if (r != LIBUSB_SUCCESS) {
		msg = "initialize usb library", ec = 1;
		goto error;
	}

	r = libusb_hotplug_register_callback(
		ctx,
		LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED,
		LIBUSB_HOTPLUG_ENUMERATE,
		USB_VID_BLACKMAGIC_DESIGN,
		LIBUSB_HOTPLUG_MATCH_ANY,
		LIBUSB_HOTPLUG_MATCH_ANY,
		handle_hotplug, NULL, &cbhandle);
	if (r != LIBUSB_SUCCESS) {
		msg = "register callback", ec = 1;
		goto error;
	}

	while (running || num_workers)
		libusb_handle_events(ctx);

error:
	if (msg)
		dlog(LOG_ERR, "failed to %s: %s", msg, libusb_error_name(r));
	if (ctx)
		libusb_exit(ctx);
	return ec;
}
