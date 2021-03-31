#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libeasymem.h>
#include <libledmgr.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#undef DONT_DISABLE
#undef NOISY
#define QUIET
#undef DAEMON_MODE
#undef LIE_ABOUT_FREQUENCY
#define FORCE_FREQUENCY_MAX
#define ASYMMETRIC_CLOCK
#define REQUIRE_IP_ADDRESS

volatile uint8_t *xvc;
#define REG32(baseptr, offset) (*((volatile uint32_t *)((baseptr) + (offset))))
#define REG8(baseptr, offset) (*((volatile uint8_t *)((baseptr) + (offset))))
#define REG_CFG 0x00
#define REG_TMS 0x04
#define REG_TDI 0x08
#define REG_TDO 0x0c
#define REG_CMD 0x10

#ifdef DAEMON_MODE
#define QUIET
#endif

#ifdef QUIET
#define dprintf(...) printf(__VA_ARGS__)
#else
#define dprintf(...)
#endif

static volatile int signal_doexit = 0;
static volatile int listenfd = -1;
void handler(int sig) {
#ifndef DONT_DISABLE
	REG32(xvc, REG_CFG) = (REG32(xvc, REG_CFG) & 0x7fffffff) | (0 << 31);
#endif
	signal_doexit = 1;
	close(listenfd);
}

static ssize_t dowrite(int fd, const void *buf, size_t count) {
	unsigned char *outbuf = (unsigned char *)buf;
	int written = 0;

	while (count > 0) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		int num = select(fd + 1, NULL, &fds, NULL, NULL);
		if (num < 0)
			return -1;
		int just_written = write(fd, outbuf, count);
		if (just_written < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			return -1;
		}
		written += just_written;
		outbuf += just_written;
		count -= just_written;
	}
	return written;
}

static ssize_t doread(int fd, void *buf, size_t count) {
	unsigned char *inbuf = (unsigned char *)buf;
	int bytesread = 0;

	while (count > 0) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		int num = select(fd + 1, &fds, NULL, NULL, NULL);
		if (num < 0)
			return -1;
		int just_read = read(fd, inbuf, count);
		if (just_read < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			return -1;
		}
		else if (just_read == 0) {
			return bytesread;
		}
		bytesread += just_read;
		inbuf += just_read;
		count -= just_read;
	}
	return bytesread;
}

class CommunicationError {};

void handle_getinfo(int clientfd) {
	const char *rsp = "xvcServer_v1.0:2048\n";
	if (dowrite(clientfd, rsp, strlen(rsp)) != static_cast<ssize_t>(strlen(rsp)))
		throw CommunicationError();
}

void handle_shift(int clientfd) {
	// our ARM is little endian.
	uint32_t nbits = 0;
	if (doread(clientfd, &nbits, sizeof(nbits)) != sizeof(nbits))
		throw CommunicationError();

#ifdef NOISY
	dprintf("Reading %u bits for shift:\n", nbits);
#endif

	int nbytes = nbits / 8 + (nbits % 8 ? 1 : 0);
	int nwords = nbytes / 4 + (nbytes % 4 ? 1 : 0);

	uint8_t tms_vec[4 * nwords];
	memset(tms_vec, 0, sizeof(tms_vec));
	if (doread(clientfd, &tms_vec, nbytes) != nbytes)
		throw CommunicationError();

	uint8_t tdi_vec[4 * nwords];
	memset(tdi_vec, 0, sizeof(tdi_vec));
	if (doread(clientfd, &tdi_vec, nbytes) != nbytes)
		throw CommunicationError();

#ifdef NOISY
	dprintf("    tms:");
	for (int i = 0; i < nbytes; i++) {
		if ((i % 16) == 0)
			dprintf("\n        ");
		else if ((i % 8) == 0)
			dprintf("   ");
		else if ((i % 4) == 0)
			dprintf(" ");
		dprintf(" %02x", tms_vec[i]);
	}
	dprintf("\n");

	dprintf("    tdi:");
	for (int i = 0; i < nbytes; i++) {
		if ((i % 16) == 0)
			dprintf("\n        ");
		else if ((i % 8) == 0)
			dprintf("   ");
		else if ((i % 4) == 0)
			dprintf(" ");
		dprintf(" %02x", tdi_vec[i]);
	}
	dprintf("\n");
#endif

	uint8_t tdo_vec[4 * nwords];

	for (int bit = 0; bit < static_cast<int>(nbits) && !signal_doexit; bit += 32) {
		int bytepos = bit / 8;

		while ((REG32(xvc, 0x010) & 0x00000200) == 0)
			; // wait for idle

		REG32(xvc, 0x004) = REG32(tms_vec, bytepos);
		REG32(xvc, 0x008) = REG32(tdi_vec, bytepos);

		REG8(xvc, 0x000) = ((nbits - bit) >= 32 ? 32 : (nbits - bit));
		REG32(xvc, 0x010) = 0x80000000;

		while ((REG32(xvc, 0x010) & 0x00000100) == 0)
			; // wait for done

		if ((nbits - bit) < 32) {
			uint32_t tdobuf = REG32(xvc, 0x00c);
			tdobuf >>= 32 - (nbits - bit);
			REG32(tdo_vec, bytepos) = tdobuf;
		}
		else {
			REG32(tdo_vec, bytepos) = REG32(xvc, 0x00c);
		}
	}

	if (signal_doexit) {
		// We disconnected the cable in the signal handler, whatever data we
		// have now is invalid.  Don't give the remote endpoint the impression
		// we succeeded at anything.
		return;
	}

#ifdef NOISY
	dprintf("    tdo:");
	for (int i = 0; i < nbytes; i++) {
		if ((i % 16) == 0)
			dprintf("\n        ");
		else if ((i % 8) == 0)
			dprintf("   ");
		else if ((i % 4) == 0)
			dprintf(" ");
		dprintf(" %02x", tdo_vec[i]);
	}
	dprintf("\n");
#endif

	if (dowrite(clientfd, &tdo_vec, nbytes) != nbytes)
		throw CommunicationError();
}

// From example project xvc_ml605
inline uint32_t byteToInteger(uint8_t *byteVector) {
	uint32_t uintRet;
	uint32_t uintRetTmp;

	uintRet = (uint32_t)(byteVector[0] & 0xff);
	uintRetTmp = (uint32_t)(byteVector[1] & 0xff);
	uintRet |= uintRetTmp << 8;
	uintRetTmp = (uint32_t)(byteVector[2] & 0xff);
	uintRet |= uintRetTmp << 16;
	uintRetTmp = (uint32_t)(byteVector[3] & 0xff);
	uintRet |= uintRetTmp << 24;

	return uintRet;
}

// From example project xvc_ml605
inline void integerToByte(uint8_t *byteVector, uint32_t uintValue) {
	uint32_t uintTemp;

	uintTemp = uintValue;
	byteVector[0] = uintTemp & 0xff;
	uintTemp = uintTemp >> 8;
	byteVector[1] = uintTemp & 0xff;
	uintTemp = uintTemp >> 8;
	byteVector[2] = uintTemp & 0xff;
	uintTemp = uintTemp >> 8;
	byteVector[3] = uintTemp & 0xff;
}

#define AXI_CLK_MHZ 50
#define AXI_CLK_PERIOD_NS (1000 / AXI_CLK_MHZ)
void handle_settck(int clientfd) {
	// our ARM is little endian.
	uint32_t reqtck = 0;
	if (doread(clientfd, &reqtck, 4) != 4)
		throw CommunicationError();

#ifndef LIE_ABOUT_FREQUENCY
	// XXX OVERRIDE FREQUENCY FOR DEBUGGING XXX
	//reqtck *= 4;

	dprintf("Clock Frequency Requested: %u ns, %u MHz\n", reqtck, (1000 / reqtck));
#ifdef FORCE_FREQUENCY_MAX
	reqtck = 1;
	dprintf("Overridden Frequency Requested: %u ns, %u MHz\n", reqtck, (1000 / reqtck));
#endif

	/* Trying to make the clock evenly divided, we will add 2 to the
	 * ckdown_width at the very end, to match what looks like a +2 on the
	 * ckup_width in this page of notes
	 */

#ifdef ASYMMETRIC_CLOCK
#define CKD_W_PAD 0
#else
#define CKD_W_PAD 2
#endif

	/* Clock Width given ClockUp Width, is the sum of:
	 *   Clock Up Period:  cku_w+2   // +2 added by xvc core
	 *   #ifndef ASYMMETRIC_CLOCK
	 *   Clock Dn Period:  cku_w+2   // +2 added by software to match ckd_w to cku_w
	 *   #else
	 *   Clock Dn Period:  cku_w     // +0 added by software, due to ASYMMETRIC_CLOCK config
	 *   #endif
	 *
	 *   Both appear to have +1 added by XVC core.  I.e. minimum wait state is 1 cycle.
	 */
#define CK_W(CKU_W) ((2 * (CKU_W)) + 2 + 1 + (CKD_W_PAD) + 1)

	uint32_t cku_w; // 0 to 253 (since we will manually +2 this for the ckd_w slot without ASYMMETRIC_CLOCK config)

	for (cku_w = 253; cku_w > 0; cku_w--) {
		if ((CK_W(cku_w - 1) * AXI_CLK_PERIOD_NS) < reqtck) {
			// Next period will be too short.  Stop here.
			break;
		}
	}
	dprintf("Resultant ckd_w: %u, cku_w: %u\n", cku_w + CKD_W_PAD, cku_w);
	dprintf("Resultant frequency: %u ns,  %u MHz\n", CK_W(cku_w) * AXI_CLK_PERIOD_NS, 1000 / (CK_W(cku_w) * AXI_CLK_PERIOD_NS));

	REG32(xvc, REG_CFG) = (REG32(xvc, REG_CFG) & 0xff0000ff) | (cku_w << 16) | ((cku_w + CKD_W_PAD) << 8);

	reqtck = CK_W(cku_w) * AXI_CLK_PERIOD_NS;
	dprintf("Set clock to %u\n", reqtck);
#else
	dprintf("Claiming I set the clock to %u.  I'm lying.  But then, so does Xilinx.\n", reqtck);
#endif

	if (dowrite(clientfd, &reqtck, 4) != 4)
		throw CommunicationError();
}

void run_client(int clientfd) {
	REG32(xvc, REG_CMD) = 0x40000000;
	REG32(xvc, REG_CFG) = (REG32(xvc, REG_CFG) & 0x7fffffff) | (1 << 31);

	char buf[10];
	memset(buf, 0, 10);
	int datalen = 0;
	int bytes;
	while (!signal_doexit && (bytes = doread(clientfd, buf + datalen, 1)) > 0) {
		datalen += bytes;
		if (datalen > 8)
			break;

		ledmgr_indicate(0x800080);

		if (datalen == 8 && strncmp(buf, "getinfo:", 8) == 0) {
			handle_getinfo(clientfd);

			memset(buf, 0, 10);
			datalen = 0;
			continue;
		}
		if (datalen == 6 && strncmp(buf, "shift:", 6) == 0) {
			handle_shift(clientfd);

			memset(buf, 0, 10);
			datalen = 0;
			continue;
		}
		if (datalen == 7 && strncmp(buf, "settck:", 7) == 0) {
			handle_settck(clientfd);

			memset(buf, 0, 10);
			datalen = 0;
			continue;
		}
	}

#ifndef DONT_DISABLE
	REG32(xvc, REG_CFG) = (REG32(xvc, REG_CFG) & 0x7fffffff) | (0 << 31);
#endif
}

void do_fork() {
	if (fork())
		exit(0);
	setsid();
	if (fork())
		exit(0);
}

void argparse_failed() __attribute__((noreturn));
void argparse_failed() {
#ifdef REQUIRE_IP_ADDRESS
	printf("xvc $uio_path $client_ip_address [$listen_port]\n");
#else
	printf("xvc $uio_path [$listen_port]\n");
#endif
	printf("\n");
	printf("The default listen_port is 2542.\n");
	exit(1);
}

int main(int argc, char *argv[]) {
	signal(SIGHUP, handler);
	signal(SIGINT, handler);
	signal(SIGTERM, handler);

	std::string uio_device;
#ifdef REQUIRE_IP_ADDRESS
	std::string client_ip;
#endif
	uint16_t listen_port = 2542;

	int argparse_i = 0;

	if (argc <= ++argparse_i)
		argparse_failed();
	uio_device = argv[argparse_i];

#ifdef REQUIRE_IP_ADDRESS
	if (argc <= ++argparse_i)
		argparse_failed();
	client_ip = argv[argparse_i];
#endif

	if (argc > ++argparse_i)
		listen_port = atoi(argv[argparse_i]);

	if (listen_port == 0) {
		printf("Bad listen_port\n\n");
		argparse_failed();
	}

	if (easymem_map_uio((void **)(&xvc), uio_device.c_str(), 0, 0x1000, 0) < 0) {
		perror("UIO mapping failed");
		return 1;
	}

	//int listenfd;
	struct sockaddr_in serv_addr;

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenfd < 0) {
		dprintf("Unable to open socket: %d (%s)\n", errno, strerror(errno));
		return 1;
	}

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(listen_port);

	int optval = 1;
	setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));

	if (bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		dprintf("Unable to bind address: %d (%s)\n", errno, strerror(errno));
		return 1;
	}

	dprintf("Listening...\n");
	listen(listenfd, 5);

#ifdef DAEMON_MODE
	do_fork();
#endif

	while (!signal_doexit) {
		struct sockaddr_in client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		int clientfd = accept(listenfd, reinterpret_cast<struct sockaddr *>(&client_addr), &client_addr_len);
		if (clientfd >= 0) {
			if (argc >= 2) {
				char remoteip[INET_ADDRSTRLEN + 1];
				memset(remoteip, 0, INET_ADDRSTRLEN + 1);
				if (inet_ntop(AF_INET, &(client_addr.sin_addr), remoteip, client_addr_len) == NULL) { // We bound to AF_INET specifically, above.
					printf("Client dropped from unknown address.\n");
					close(clientfd);
					continue;
				}
				printf("Client connected from %s\n", remoteip);
#ifdef REQUIRE_IP_ADDRESS
				if (client_ip != remoteip) {
					printf("Client rejected.  We will only accept connections from \"%s\"\n", argv[1]);
					close(clientfd);
					continue;
				}
#endif
			}

			int optval = 3; // tcp_keepalive_probes (max unacknowledged)
			if (setsockopt(clientfd, IPPROTO_TCP, TCP_KEEPCNT, &optval, sizeof(optval)) < 0) {
				dprintf("Failed to set TCP_KEEPCNT.  Dropping new client\n");
				close(clientfd);
				continue;
			}

			optval = 60; // tcp_keepalive_time (idle time before connection's first probe)
			if (setsockopt(clientfd, IPPROTO_TCP, TCP_KEEPIDLE, &optval, sizeof(optval)) < 0) {
				dprintf("Failed to set TCP_KEEPIDLE.  Dropping new client\n");
				close(clientfd);
				continue;
			}

			optval = 60; // tcp_keepalive_intvl (time between probes, data or not)
			if (setsockopt(clientfd, IPPROTO_TCP, TCP_KEEPINTVL, &optval, sizeof(optval)) < 0) {
				dprintf("Failed to set TCP_KEEPINTVL.  Dropping new client\n");
				close(clientfd);
				continue;
			}

			optval = 1; // enable tcp keepalive
			if (setsockopt(clientfd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0) {
				dprintf("Failed to set SO_KEEPALIVE.  Dropping new client\n");
				close(clientfd);
				continue;
			}

			try {
				run_client(clientfd);
				dprintf("Client disconnected.\n");
			}
			catch (CommunicationError &e) {
				dprintf("Communication Error.  Client connection terminated.\n");
			}
#ifndef DONT_DISABLE
			REG32(xvc, REG_CFG) = (REG32(xvc, REG_CFG) & 0x7fffffff) | (0 << 31);
#endif
			close(clientfd);
		}
	}
	close(listenfd);
	return 0;
}
