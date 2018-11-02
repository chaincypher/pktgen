#define _GNU_SOURCE // for recvmmsg

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"


#define MTU_SIZE (2048-64*2)
#define MAX_MSG 1024

struct state {
	int fd;
	int cpu_id;
	volatile uint64_t bps;
	volatile uint64_t pps;
	struct mmsghdr messages[MAX_MSG];
	char buffers[MAX_MSG][MTU_SIZE];
	struct iovec iovecs[MAX_MSG];
} __attribute__ ((aligned (64)));

struct state *state_init(struct state *s) {
	int i;
	for (i = 0; i < MAX_MSG; i++) {
		char *buf = &s->buffers[i][0];
		struct iovec *iovec = &s->iovecs[i];
		struct mmsghdr *msg = &s->messages[i];

		msg->msg_hdr.msg_iov = iovec;
		msg->msg_hdr.msg_iovlen = 1;

		iovec->iov_base = buf;
		iovec->iov_len = MTU_SIZE;
	}
	return s;
}

static void thread_loop(void *userdata)
{
	struct state *state = userdata;

	while (1) {
		/* Blocking recv. */
		int r = recvmmsg(state->fd, &state->messages[0], MAX_MSG, 0, NULL);
		if (r <= 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				continue;
			}
			PFATAL("recvmmsg()");
		}

		int i, bytes = 0;
		for (i = 0; i < r; i++) {
			struct mmsghdr *msg = &state->messages[i];
			/* char *buf = msg->msg_hdr.msg_iov->iov_base; */
			int len = msg->msg_len;
			msg->msg_hdr.msg_flags = 0;
			msg->msg_len = 0;
			bytes += len;
		}
		__atomic_fetch_add(&state->pps, r, 0);
		__atomic_fetch_add(&state->bps, bytes, 0);
	}
}

int main(int argc, const char *argv[])
{
	int recv_buf_size = 4*1024*1024;
	int thread_num = argc-1;

	if (argc ==1) {
		FATAL("Usage: %s [listen ip:port]", argv[0]);
	}
        struct net_addr *listen_addrs = calloc(thread_num, sizeof(struct net_addr));

        int t;
        for (t = 0; t < thread_num; t++) {
                const char *listen_addr_str = argv[t+1];
                parse_addr(&listen_addrs[t], listen_addr_str);
        }

	struct state *array_of_states = calloc(thread_num, sizeof(struct state));

	for (t = 0; t < thread_num; t++) {
		struct state *state = &array_of_states[t];
		state_init(state);
		fprintf(stderr, "[*] Starting udpreceiver on %s, recv buffer %iKiB\n",
			addr_to_str(&listen_addrs[t]), recv_buf_size / 1024);

		int fd = net_bind_udp(&listen_addrs[t], 1);
		net_set_buffer_size(fd, recv_buf_size, 0);
		state->fd = fd;
		state->cpu_id = t;
		thread_spawn(thread_loop, state, 1, state->cpu_id);
	}

	uint64_t last_pps = 0;
	uint64_t last_bps = 0;
	uint64_t total_packets = 0;
	while (1) {
		struct timeval timeout =
			NSEC_TIMEVAL(MSEC_NSEC(1000UL));
		while (1) {
			int r = select(0, NULL, NULL, NULL, &timeout);
			if (r != 0) {
				continue;
			}
			if (TIMEVAL_NSEC(&timeout) == 0) {
				break;
			}
		}

		uint64_t now_pps = 0, now_bps = 0;
		for (t = 0; t < thread_num; t++) {
			struct state *state = &array_of_states[t];
			now_pps += __atomic_load_n(&state->pps, 0);
			now_bps += __atomic_load_n(&state->bps, 0);
		}

		total_packets += now_pps - last_pps;
		double delta_pps = now_pps - last_pps;
		double delta_bps = now_bps - last_bps;
		last_pps = now_pps;
		last_bps = now_bps;

		printf("%7.3fM pps %7.3fMiB / %7.3fMb Total %lu packets\n",
		       delta_pps / 1000.0 / 1000.0,
		       delta_bps / 1024.0 / 1024.0,
		       delta_bps * 8.0 / 1000.0 / 1000.0,
		       total_packets );
	}

	return 0;
}
