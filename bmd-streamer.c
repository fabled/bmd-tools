/* BlackMagic Design tools - h264 stream over USB extractor
 * - ATEM TV Studio
 * - H264 Pro Encoder (not tested)
 *
 * These devices seem to use Fujitsu MB86H56 for encoding HD H264 video.
 * Getting technical datasheet to that chip would make things a lot clearer.
 */

#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <libusb.h>

#include "blackmagic.h"

/*
 * TODO and ideas:
 * - AIN_OFFSET, VR_SET_AUDIO_DELAY: test
 * - Handle resolution changes properly
 * - Selecting capture target format - now it's "Native (Progressive)"
 * - Per-device configuration to allow sending multiple streams to
 *   different sockets
 * - Consider rewriting the threaded implementation to state machines
 *   and asynchronous USB library usage. This would remove the few long
 *   timeouts while ending playback.
 */

#define array_size(x) (sizeof(x) / sizeof(x[0]))

static int running = 1;
static volatile int num_workers = 0;

enum DISPLAY_MODE {
	DMODE_720x480i_29_97 = 0,
	DMODE_720x576i_30,
	DMODE_invalid,
	DMODE_720x480p_59_94,
	DMODE_720x576p_50,
	DMODE_1920x1080p_23_976 = 5,
	DMODE_1920x1080p_24,
	DMODE_1920x1080p_25,
	DMODE_1920x1080p_29_97,
	DMODE_1920x1080p_30,
	DMODE_1920x1080i_25,
	DMODE_1920x1080i_29_97, /* b */
	DMODE_1920x1080i_30,
	DMODE_1920x1080p_50,
	DMODE_1920x1080p_59_94,
	DMODE_1920x1080p_60,
	DMODE_1280x720p_50 = 0x10,
	DMODE_1280x720p_59_94,
	DMODE_1280x720p_60,
	DMODE_MAX
};

struct display_mode {
	const char *	description;
	int		width, height;
	int		fps_numerator, fps_denominator;

	uint8_t		interlaced;
	uint8_t		fx2_fps;

	uint16_t	ain_offset;
	uint16_t	r1000, r140a_l, r1404;
	uint16_t	r147x[4];
	uint16_t	r154x[10];
};

struct encoding_parameters {
	uint16_t	video_kbps, video_max_kbps, audio_kbps, audio_khz;
	uint8_t		h264_profile, h264_level, h264_bframes, h264_cabac;
};

static struct display_mode *display_modes[DMODE_MAX] = {
	[DMODE_1920x1080i_25] = &(struct display_mode){
		.description = "1080i 50",
		.width = 1920, .height = 1080, .interlaced = 1,
		.fps_numerator = 25, .fps_denominator = 1, .fx2_fps = 0x3,
		.ain_offset = 0x0000,
		.r1000 = 0x0200, .r1404 = 0x0041, .r140a_l = 0x01,
		.r147x = { 0x26, 0x7d, 0x56, 0x07 },
		.r154x = { 0x0034, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x000e, 0x0780, 0x0438, 0x0000 },
	},
	[DMODE_1280x720p_50] = &(struct display_mode){
		.description = "720p 50",
		.width = 1280, .height = 720, .interlaced = 0,
		.fps_numerator = 50, .fps_denominator = 1, .fx2_fps = 0x6,
		.ain_offset = 0x0384,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a_l = 0xff,
		.r147x = { 0x10, 0x70, 0x70, 0x10 },
		.r154x = { 0x0001, 0x07ff, 0x07bb, 0x02ee, 0x0107, 0x001a, 0x07ff, 0x0500, 0x02d0, 0x0032 },
	},
	[DMODE_1920x1080i_29_97] = &(struct display_mode){
		.description = "1080i 29.97",
		.width = 1920, .height = 1080, .interlaced = 1,
		.fps_numerator = 30000, .fps_denominator = 1001, .fx2_fps = 0x4,
		.ain_offset = 0x0000,
		.r1000 = 0x0200, .r1404 = 0x0071, .r140a_l = 0x00,
		.r147x = { 0x26, 0x7d, 0x56, 0x07 },
		.r154x = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x000e, 0x0000, 0x0400, 0x0000 },
	},
	[DMODE_1280x720p_59_94] = &(struct display_mode){
		.description = "720p 59.94",
		.width = 1280, .height = 720, .interlaced = 0,
		.fps_numerator = 60000, .fps_denominator = 1001, .fx2_fps = 0x7,
		.ain_offset = 0x0384,
		.r1000 = 0x0500, .r1404 = 0x0071, .r140a_l = 0xff,
		.r147x = { 0x10, 0x70, 0x70, 0x10 },
		.r154x = { 0x0001, 0x07ff, 0x07bb, 0x02ee, 0x0107, 0x001a, 0x07ff, 0x0500, 0x02d0, 0x003c },
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
	case 0x20: return DMODE_720x576i_30;
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

	r = stat(filename, &st);
	if (r != 0) {
		fprintf(stderr, "%s: failed to load firmware to memory\n", filename);
		return NULL;
	}

	fw = malloc(sizeof(struct firmware) + st.st_size);
	if (fw == NULL)
		return NULL;

	fw->size = st.st_size;
	fw->device_id = device_id;

	fd = open(filename, O_RDONLY);
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
	int oldlen;
	unsigned char olddata[0xbc];
	unsigned char data[16*1024];
};

static void mpegparser_parse(struct mpeg_parser_buffer *pb, int newlen)
{
	unsigned char *buf = &pb->data[-pb->oldlen];
	int i = 0, len = newlen + pb->oldlen;

	while (i + 0xbc <= len) {
		if (memcmp(&buf[i], "\x00\x00\x00\x00", 4) == 0) {
			i += 0xbc;
			continue;
		}
		if (buf[i] != 0x47) {
			while (buf[i] != 0x47 && i < len)
				i++;
			continue;
		}
		if (buf[i+1] == 0x1f && buf[i+2] == 0xff) {
			i += 0xbc;
			continue;
		}
		write(STDOUT_FILENO, &buf[i], 0xbc);
		i += 0xbc;
	}

	pb->oldlen = len - i;
	memcpy(&pb->data[-pb->oldlen], &buf[i], pb->oldlen);
}

struct blackmagic_device {
	char name[64];
	pthread_t device_thread, mpegts_thread;
	volatile int running;
	int status;
	int fxstatus;
	int recognized;
	int encode_sent;
	struct display_mode *current_mode;

	struct libusb_device_descriptor desc;
	libusb_device *usbdev;
	libusb_device_handle *usbdev_handle;

	uint8_t mac[6];

	uint8_t message_buffer[1024];
	struct mpeg_parser_buffer mpegparser;
};

static void dostop(int sig)
{
	running = 0;
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

#if 0
	{
		uint16_t oldvalue = bmd_fujitsu_read(bmd, reg);
		if (value != oldvalue)
			fprintf(stderr, "%s: fujitsu_write @%06x %04x != %04x\n", bmd->name, reg, value, oldvalue);
	}
#endif

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

		mpegparser_parse(&bmd->mpegparser, actual_length);
	} while (running && bmd->running);

	fprintf(stderr, "%s: mpeg-ts pump exiting: %s\n", bmd->name, libusb_error_name(r));

	return NULL;
}

static int bmd_recognize_device(struct blackmagic_device *bmd)
{
	int i;

	/* 0x84..0x87	firmware version, timestamp or checksum
	 *	4.1.2 -> 71 70 86 c6
	 *	4.2.1 -> 7a 01 f1 34
	 * 0x88..0x8d	mac address
	 * 0xa0..0xa3	ipv4 address
	 * 0xa4..0xa7	ipv4 netmask
	 * 0xa8..0xab	ipv4 gateway
	 */

	for (i = 0; i < 6; i++)
		bmd_read_register(bmd, 0x88 + i, &bmd->mac[i]);

	fprintf(stderr, "%s: MAC address %02x:%02x:%02x:%02x:%02x:%02x\n",
		bmd->name,
		bmd->mac[0], bmd->mac[1], bmd->mac[2],
		bmd->mac[3], bmd->mac[4], bmd->mac[5]);

	bmd->recognized = 1;

	return bmd->status == LIBUSB_SUCCESS;
}

static int bmd_configure_encoder(struct blackmagic_device *bmd, struct encoding_parameters *ep)
{
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
		VR_SET_AUDIO_DELAY, 0, 0, "\x00", 1, 5000);
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
	bmd_fujitsu_write(bmd, 0x001000, current_mode->r1000); // 0x200=1080i, 0x500=720p
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
	// r140a_l == 1=1080i 4=NTSC, 5=PAL, 0xff=720p
	bmd_fujitsu_write(bmd, 0x00140a, 0x1700 | current_mode->r140a_l);
	bmd_fujitsu_write(bmd, 0x00140c, ep->h264_cabac ? 0x0000 : 0x0100);
	bmd_fujitsu_write(bmd, 0x00140e, 0xd400 | ((current_mode->fps_denominator == 1) ? 0x0001 : 0x0000));
	bmd_fujitsu_write(bmd, 0x001418, 0x0001);
	bmd_fujitsu_write(bmd, 0x001420, 0x0000);
	bmd_fujitsu_write(bmd, 0x001422, ep->video_max_kbps);
	/* Register 0x1430 lower byte is related to INPUT MODE/TARGET MODE specific.
	 *  affects directly the output stream resolution, possibly TS mode bits. */
	bmd_fujitsu_write(bmd, 0x001430, 0xff | (ep->h264_bframes ? 0x0000 : 0x0100));
	bmd_fujitsu_write(bmd, 0x001470, current_mode->r147x[0]);
	bmd_fujitsu_write(bmd, 0x001472, current_mode->r147x[1]);
	bmd_fujitsu_write(bmd, 0x001474, current_mode->r147x[2]);
	bmd_fujitsu_write(bmd, 0x001476, current_mode->r147x[3]);
	bmd_fujitsu_write(bmd, 0x001478, 0x0000);
	bmd_fujitsu_write(bmd, 0x00147a, 0x0000);
	bmd_fujitsu_write(bmd, 0x00147c, 0x0000);
	bmd_fujitsu_write(bmd, 0x00147e, 0x0000);
	/* 0x001540 depends on if PAL/NTSC or interlaced mode is set */
	bmd_fujitsu_write(bmd, 0x001540, 0x0000);

	/* INPUT MODE based constants likely tuning for sync or similar,
	 * some of these seem to get ignored (and initialized to random
	 * value by the BMD drivers). */
	bmd_fujitsu_write(bmd, 0x001542, current_mode->r154x[0]);
	bmd_fujitsu_write(bmd, 0x001544, current_mode->r154x[1]);
	bmd_fujitsu_write(bmd, 0x001546, current_mode->r154x[2]);
	bmd_fujitsu_write(bmd, 0x001548, current_mode->r154x[3]);
	bmd_fujitsu_write(bmd, 0x00154a, current_mode->r154x[4]);
	bmd_fujitsu_write(bmd, 0x00154c, current_mode->r154x[5]);
	bmd_fujitsu_write(bmd, 0x00154e, current_mode->r154x[6]);
	bmd_fujitsu_write(bmd, 0x001550, current_mode->r154x[7]);
	bmd_fujitsu_write(bmd, 0x001552, current_mode->r154x[8]);
	bmd_fujitsu_write(bmd, 0x001554, current_mode->r154x[9]);

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
	} else if (current_mode->height == 1080) {
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
	bmd_fujitsu_write(bmd, 0x0015a8, 2 * current_mode->fps_numerator >> 16);
	bmd_fujitsu_write(bmd, 0x0015aa, 2 * current_mode->fps_numerator & 0xffff);
	bmd_fujitsu_write(bmd, 0x0015ac, 0x0001); // {1=HD,2=PAL,3=NTSC} depends on target resolution
	bmd_fujitsu_write(bmd, 0x0015b2, 0); // bit 0x8000 = set if source is 1280x720 and fps differs
	#if 0
	if (0 /* source is 1280x720 and fps differs */) {
		/* Possibly input-output frame ratio */
		bmd_fujitsu_write(bmd, 0x0015b2, 2);
		bmd_fujitsu_write(bmd, 0x0015b4, 1);
	}
	#endif

	/* Group 6 - Enable */
	bmd_fujitsu_write(bmd, 0x001144, 0x3333);

	return bmd->status == LIBUSB_SUCCESS;
}

static void bmd_encoder_start(struct blackmagic_device *bmd)
{
	static struct encoding_parameters ep = {
		.video_kbps = 3000,
		.video_max_kbps = 3500,
		.h264_profile = FX2_H264_HIGH,
		.h264_level = 40,
		.h264_cabac = 1,
		.audio_kbps = 256,
		.audio_khz = 48000,
	};
	const char *err;
	uint8_t status;
	int r;

	bmd->encode_sent = 1;

	fprintf(stderr, "%s: Configuring and starting encoder\n", bmd->name);
	bmd_configure_encoder(bmd, &ep);

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
	fprintf(stderr, "%s: failed to %s\n", bmd->name, err);
}

static void bmd_encoder_stop(struct blackmagic_device *bmd)
{
	uint8_t status;
	uint8_t clear_fpga_command[1] = { 0x02 };
	uint8_t send_fpga_command[1] = { 0x80 };
	uint32_t fifo_level;
	int i, r;

	/* Stop recording */
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

static void bmd_parse_message(struct blackmagic_device *bmd, const uint8_t *msg)
{
	int dm;

	switch (msg[0]) {
	case 0x01: /* Status update */
		fprintf(stderr, "%s: FX2Status: %s (%d)\n", bmd->name, FX2Status_to_String(msg[5]), msg[5]);
		bmd->fxstatus = msg[5];

		if (bmd->fxstatus == FX2Status_Idle) {
			bmd->encode_sent = 0;
			if (!bmd->recognized)
				bmd_recognize_device(bmd);
			if (bmd->running && bmd->current_mode && !bmd->encode_sent)
				bmd_encoder_start(bmd);
		}
		break;
	case 0x05: /* Input connector */
		dm = input_mode_to_display_mode(msg[1]);
		bmd->current_mode = display_modes[dm];

		if (bmd->current_mode)
			fprintf(stderr, "%s: Display Mode: %s\n", bmd->name, bmd->current_mode->description);
		else
			fprintf(stderr, "%s: Input Mode, 0x%02x (display mode 0x%02x) not supported\n", bmd->name, msg[1], dm);

		if (bmd->running && bmd->fxstatus == FX2Status_Idle && bmd->current_mode && !bmd->encode_sent)
			bmd_encoder_start(bmd);
		break;
	case 0x0d:
		fprintf(stderr, "%s: H56 Error. Restarting device.\n", bmd->name);
		break;
	case 0x0e: /* Timestamp update? */
		break;
	}
}

static void bmd_handle_messages(struct blackmagic_device *bmd)
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
		fprintf(stderr, "EP8: %4d bytes:", actual_length);
		for (i = 0; i < actual_length; i++)
			fprintf(stderr, " %02x", bmd->message_buffer[i]);
		fprintf(stderr, "\n");

		for (i = 2; bmd->message_buffer[i] != 0 && i < actual_length;
		     i += bmd->message_buffer[i] + 1)
			bmd_parse_message(bmd, &bmd->message_buffer[i+1]);
	} while (running && bmd->running);

	if (r != LIBUSB_SUCCESS) {
		fprintf(stderr, "%s: message reader exiting: %s\n", bmd->name, libusb_error_name(r));
		bmd->status = r;
	}
}

static void *bmd_device_thread(void *ctx)
{
	struct blackmagic_device *bmd = ctx;
	int r, i;

	bmd->running = 1;

	/* Immediately after hotplug, the sysfs device nodes are not yet
	 * available. Unfortunately, libusb_open will disconnect mark device
	 * disconnected if the node does not exist... so just wait for
	 * (hopefully) long enough for the node to become available. */
	usleep(200 * 1000);

	r = libusb_open(bmd->usbdev, &bmd->usbdev_handle);
	if (r != LIBUSB_SUCCESS) {
		fprintf(stderr, "%s: unable to open device: %s\n", bmd->name, libusb_error_name(r));
		goto exit;
	}

	r = libusb_set_configuration(bmd->usbdev_handle, 1);
	if (r != LIBUSB_SUCCESS) {
		fprintf(stderr, "%s: failed to set configuration: %s\n", bmd->name, libusb_error_name(r));
		goto exit;
	}

	r = libusb_claim_interface(bmd->usbdev_handle, 0);
	if (r != LIBUSB_SUCCESS) {
		fprintf(stderr, "%s: failed to claim interface: %s\n", bmd->name, libusb_error_name(r));
		goto exit;
	}

	if (bmd->desc.iManufacturer == 0) {
		const char *desc = "not available";

		fprintf(stderr, "%s: firmware downloaded needed\n", bmd->name);
		for (i = 0; i < array_size(firmwares); i++) {
			if (firmwares[i]->device_id != bmd->desc.idProduct)
				continue;
			if (bmd_upload_firmware(bmd, firmwares[i]))
				desc = "downloaded succesfully";
			else
				desc = "failed to download";
			break;
		}
		fprintf(stderr, "%s: firmware %s\n", bmd->name, desc);
	} else {
		r = pthread_create(&bmd->mpegts_thread, NULL, bmd_pump_mpegts, bmd);
		if (r < 0)
			goto exit;

		r = libusb_control_transfer(
			bmd->usbdev_handle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
			VR_SEND_DEVICE_STATUS, 0, 0, 0, 0, 1000);
		if (r == LIBUSB_SUCCESS)
			bmd_handle_messages(bmd);

		bmd->running = 0;
		if (bmd->mpegts_thread)
			pthread_join(bmd->mpegts_thread, NULL);

		if (bmd->fxstatus == FX2Status_Encoding && bmd->status == LIBUSB_SUCCESS) {
			bmd_encoder_stop(bmd);
			while (bmd->fxstatus != FX2Status_Idle && bmd->status == LIBUSB_SUCCESS)
				bmd_handle_messages(bmd);
		}
	}

exit:
	fprintf(stderr, "%s: closing device\n", bmd->name);
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

	fprintf(stderr, "%s: device connected\n", bmd->name);

	__sync_add_and_fetch(&num_workers, 1);

	r = pthread_create(&bmd->device_thread, NULL, bmd_device_thread, bmd);
	if (r != 0) {
		fprintf(stderr, "%s: failed to create handler thread\n", bmd->name);
		libusb_unref_device(bmd->usbdev);
		free(bmd);
		return 0;
	}
	pthread_detach(bmd->device_thread);

	return 0;
}

int main(void)
{
	libusb_context *ctx;
	libusb_hotplug_callback_handle cbhandle;
	const char *msg = NULL;
	int i, r, ec = 0;

	signal(SIGTERM, dostop);
	signal(SIGINT, dostop);

	firmwares[0] = load_firmware("bmd-atemtvstudio.bin", USB_PID_BMD_ATEM_TV_STUDIO);
	firmwares[1] = load_firmware("bmd-h264prorecorder.bin", USB_PID_BMD_H264_PRO_RECORDER);

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
		fprintf(stderr, "Failed to %s: %s\n", msg, libusb_error_name(r));
	if (ctx)
		libusb_exit(ctx);
	return ec;
}
