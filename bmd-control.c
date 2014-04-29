/* BlackMagic Design tools - Remote control for mixer
 *
 * protocol insight derived from:
 * https://github.com/petersimonsson/libqatemcontrol
 */

#define _GNU_SOURCE
#include <poll.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define __STRINGIFY(s) #s
#define STRINGIFY(s) __STRINGIFY(s)
#define count_of(x) (sizeof(x) / sizeof((x) [0]))

#define ATEM_UDP_PORT		9910

#define ATEM_CMD_ACK		0x8000
#define ATEM_CMD_RESEND		0x2000
#define ATEM_CMD_HELLO		0x1000
#define ATEM_CMD_ACKREQ		0x0800
#define ATEM_CMD_LENGTHMASK	0x07ff

struct atem_cmd_header {
	uint16_t	length;
	uint16_t	uid;
	uint16_t	ack_id;
	uint16_t	unknown[2];
	uint16_t	packet_id;
};

struct atem_subcmd_header {
	uint16_t	length;
	uint16_t	unknown;
	uint32_t	type;
};

struct atem_connection {
	int		fd;
	uint16_t	uid;
	uint16_t	packet_id;
};

static void send_hello(struct atem_connection *c)
{
	struct {
		struct atem_cmd_header h;
		char hello[8];
	} msg = {
		.h.length = htons(ATEM_CMD_HELLO | sizeof(msg)),
		.h.uid = htons(0x1337),
		.hello = { 0x01 },
	};
	send(c->fd, &msg, sizeof(msg), 0);
}

static void send_ack(struct atem_connection *c, struct atem_cmd_header *hdr)
{
	struct atem_cmd_header ack = (struct atem_cmd_header) {
		.length = htons(ATEM_CMD_ACK | sizeof(ack)),
		.uid = c->uid,
		.ack_id = hdr->packet_id,
	};
	send(c->fd, &ack, sizeof(ack), 0);
}

static struct atem_subcmd_header *next_subhdr(struct atem_cmd_header *hdr, struct atem_subcmd_header *sub)
{
	void *nxt, *end;
	nxt = ((void *) sub) + ntohs(sub->length);
	end = ((void *) hdr) + (ntohs(hdr->length) & ATEM_CMD_LENGTHMASK);
	if (nxt >= end) return NULL;
	return nxt;
}

static void handle_atem_messages(struct atem_connection *c)
{
	union {
		char buf[1500];
		struct {
			struct atem_cmd_header h;
			struct atem_subcmd_header first;
		};
	} u;
	struct atem_subcmd_header *sub;
	ssize_t len;

	len = recv(c->fd, u.buf, sizeof(u.buf), 0);
	if (len < (ssize_t) sizeof(u.h))
		return;

	fprintf(stderr, "ATEM: Received %d bytes\n", len);
	c->uid = u.h.uid;
	if (u.h.length & htons(ATEM_CMD_HELLO | ATEM_CMD_ACKREQ))
		send_ack(c, &u.h);
	if (u.h.length & htons(ATEM_CMD_HELLO))
		return;
	if (len < (ssize_t) sizeof(u.h)+sizeof(u.first))
		return;
	for (sub = &u.first; sub; sub = next_subhdr(&u.h, sub)) {
		fprintf(stderr, "  -> type: %4.4s, length: %d\n",
			&sub->type, ntohs(sub->length));
	}
}

static void handle_client_messages(int cfd, struct atem_connection *c)
{
	union {
		char buf[1500];
		struct {
			struct atem_cmd_header h;
			struct atem_subcmd_header first;
		};
	} u;
	ssize_t len;

	len = recv(cfd, u.buf, sizeof(u.buf), 0);
	if (len < (ssize_t) sizeof(u.h))
		return;

	fprintf(stderr, "CLNT: Forwarding %d bytes to ATEM\n", len);
	u.h = (struct atem_cmd_header) {
		.length = htons(ATEM_CMD_ACKREQ | len),
		.uid = c->uid,
		.packet_id = htons(c->packet_id++),
	};
	send(c->fd, u.buf, len, 0);
}

#define B_U16(x) (((x) >> 8) & 0xff), ((x) & 0xff)
#define B_U32(x) B_U16((x) >> 16), B_U16(x)
#define ATEMCMD(str, data...) \
{ str, (uint8_t []) { B_U16(4+sizeof((uint8_t[]){data})), B_U16(0), data } }

static const struct atem_command {
	char *name;
	uint8_t *data;
} atem_commands[] = {
	ATEMCMD("auto",			'D','A','u','t', B_U32(0)),
	ATEMCMD("preview:1",		'C','P','v','I', B_U16(0), B_U16(1)),
	ATEMCMD("preview:2",		'C','P','v','I', B_U16(0), B_U16(2)),
	ATEMCMD("preview:3",		'C','P','v','I', B_U16(0), B_U16(3)),
	ATEMCMD("preview:4",		'C','P','v','I', B_U16(0), B_U16(4)),
	ATEMCMD("preview:5",		'C','P','v','I', B_U16(0), B_U16(5)),
	ATEMCMD("preview:6",		'C','P','v','I', B_U16(0), B_U16(6)),
	ATEMCMD("preview:bars",		'C','P','v','I', B_U16(0), B_U16(1000)),
	ATEMCMD("preview:color1",	'C','P','v','I', B_U16(0), B_U16(2001)),
	ATEMCMD("preview:color2",	'C','P','v','I', B_U16(0), B_U16(2002)),
	ATEMCMD("preview:media1",	'C','P','v','I', B_U16(0), B_U16(3010)),
	ATEMCMD("preview:media2",	'C','P','v','I', B_U16(0), B_U16(3020)),
};

static int cmpcmd(const void *m1, const void *m2)
{
	const struct atem_command *c1 = m1, *c2 = m2;
	return strcmp(c1->name, c2->name);
}

static int send_commands(struct atem_connection *c, int argc, char **argv)
{
	union {
		char buf[1500];
		struct atem_cmd_header h;
	} u;
	struct atem_subcmd_header *sh;
	int i, sub_len, pos = sizeof(struct atem_cmd_header);

	if (argc < 2) return -1;

	for (i = 1; i < argc; i++) {
		struct atem_command key, *res;
		key.name = argv[i];
		res = bsearch(&key, atem_commands, count_of(atem_commands),
			      sizeof(struct atem_command), cmpcmd);
		if (!res) {
			fprintf(stderr, "Unrecognized command: %s\n", argv[i]);
			return -1;
		}
		sh = (struct atem_subcmd_header *) res->data;
		sub_len = htons(sh->length);
		if (pos + sub_len >= sizeof(u.buf)) {
			fprintf(stderr, "Too many commands\n");
			return -1;
		}
		memcpy(&u.buf[pos], sh, sub_len);
		pos += sub_len;
	}

	u.h = (struct atem_cmd_header) {
		.length = htons(ATEM_CMD_ACKREQ | pos),
		.uid = c->uid,
		.packet_id = htons(++c->packet_id),
	};
	send(c->fd, u.buf, pos, 0);

	return 0;
}

static int usage(void)
{
	fprintf(stderr, "usage: bmd-control [-d|--daemon] [-c|--connect HOST] [CMD]\n");
	return 1;
}

int main(int argc, char **argv)
{
	static const struct option long_options[] = {
		{ "connect", no_argument, NULL, 'c' },
		{ "daemon", no_argument, NULL, 'd' },
		{ NULL }
	};
	static const char short_options[] = "c:d";

	struct sockaddr_in sa;
	struct atem_connection c;
	struct pollfd fds[2];
	struct addrinfo *result, *rp;
	const char *connect_host = NULL;
	int daemon = 0, ret = 10, rc, lfd, opt, optindex;

	optindex = 0;
	while ((opt=getopt_long(argc, argv, short_options, long_options, &optindex)) > 0) {
		switch (opt) {
		case 'c':
			connect_host = optarg;
			break;
		case 'd':
			daemon = 1;
			break;
		default:
			return usage();
		}
	}
	if (!daemon && !connect_host) connect_host = "127.0.0.1";

	if (daemon) {
		lfd = socket(AF_INET, SOCK_DGRAM, 0);
		fcntl(lfd, F_SETFL, O_NONBLOCK);
		if (lfd < 0) {
			perror("socket");
			return 1;
		}

		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sa.sin_port = htons(ATEM_UDP_PORT);
		if (bind(lfd, (const struct sockaddr*) &sa, sizeof(sa)) < 0) {
			perror("bind");
			return 1;
		}
	}

	rc = getaddrinfo(connect_host, STRINGIFY(ATEM_UDP_PORT), NULL, &result);
	if (rc != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
		return 2;
	}

	memset(&c, 0, sizeof(c));
	c.fd = -1;
	for (rp = result; rp; rp = rp->ai_next) {
		int fd = socket(rp->ai_family, SOCK_DGRAM, 0);
		if (fd < 0) continue;
		fcntl(fd, F_SETFL, O_NONBLOCK);
		if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
			c.fd = fd;
			break;
		}
		perror("connect");
		close(fd);
	}
	freeaddrinfo(result);
	if (c.fd < 0) {
		fprintf(stderr, "failed to connect to atem\n");
		return 2;
	}

	ret = 0;
	if (daemon) {
		fds[0].fd = c.fd;
		fds[0].events = POLLIN;
		fds[1].fd = lfd;
		fds[1].events = POLLIN;

		send_hello(&c);
		while (1) {
			poll(fds, count_of(fds), -1);
			if (fds[0].revents & POLLIN)
				handle_atem_messages(&c);
			if (fds[1].revents & POLLIN)
				handle_client_messages(lfd, &c);
		}
	} else {
		if (send_commands(&c, argc, argv) < 0)
			ret = usage();
	}

	close(c.fd);
	return ret;
}
