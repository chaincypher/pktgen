#define _GNU_SOURCE // sendmmsg

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <time.h>

#include "common.h"

int total_packets=0;

int pause(int waitus)
{
   struct timespec tim, tim2;
   tim.tv_sec = 0;
   tim.tv_nsec = waitus * 1000;

   if(nanosleep(&tim , &tim2) < 0 )   
   {
      printf("Nano sleep system call failed \n");
      return -1;
   }

   return 0;
}

struct state {
	struct net_addr *target_addr;
	int packets_in_buf;
	const char *payload;
	int payload_sz;
	int src_port;
	int cpu_id;
	int waitus;
	int packets;
	uint64_t bytes;
	int cnt;
};

void thread_loop(void *userdata) {
	struct state *state = userdata;

	struct mmsghdr *messages = calloc(state->packets_in_buf, sizeof(struct mmsghdr));
	struct iovec *iovecs = calloc(state->packets_in_buf, sizeof(struct iovec));

	int fd = net_connect_udp(state->target_addr, state->src_port);

	int i;
	for (i = 0; i < state->packets_in_buf; i++) {
		struct iovec *iovec = &iovecs[i];
		struct mmsghdr *msg = &messages[i];

		msg->msg_hdr.msg_iov = iovec;
		msg->msg_hdr.msg_iovlen = 1;

		iovec->iov_base = (void*)state->payload;
		iovec->iov_len = state->payload_sz;
	}
	int cnt = state->cnt;
	while (cnt) {
		int r = sendmmsg(fd, messages, state->packets_in_buf, 0);
		if (r <= 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				continue;
			}

			if (errno == ECONNREFUSED) {
				continue;
			}
			PFATAL("sendmmsg()");
		}
		int i, bytes = 0;
		for (i = 0; i < r; i++) {
			struct mmsghdr *msg = &messages[i];
			/* char *buf = msg->msg_hdr.msg_iov->iov_base; */
			int len = msg->msg_len;
			msg->msg_hdr.msg_flags = 0;
			msg->msg_len = 0;
			bytes += len;
		}
		state->bytes += bytes;
		state->packets += r;
		cnt--;
		pause(state->waitus);
	}
}

// pps: packet/s, frame_size: ethernet frame, burst_size: packet burst num
// thread_num: number of threads, line_rate: bps in M (bits per us)
int calculate_pause (int pps, int frame_size, int burst_size, int thread_num, int line_rate) {
	uint64_t bits_per_burst = (frame_size+20)*burst_size*8;
	uint64_t burst_per_thread = pps / thread_num / burst_size;
	uint64_t pause_time = (line_rate * 1000000 - (burst_per_thread * bits_per_burst))/line_rate / burst_per_thread * 0.6;
	return (int)pause_time;
}

int main(int argc, const char *argv[])
{
	if (argc == 1) {
		FATAL("Usage: %s frame_size burst count pps(k) [target ip:port] [target ...]", argv[0]);
	}
	int frame_size = atoi(argv[1]);	// ethernet frame size
	int payload_sz = frame_size - 18 -20 -8;	// udp payload size
	int burst_size = atoi(argv[2]);
	int loop_count = atoi(argv[3]);
	int pps = atoi(argv[4]) * 1000;
	int packets_in_buf = burst_size;
	const char *payload = calloc(payload_sz, 1);
	struct net_addr *target_addrs = calloc(argc-5, sizeof(struct net_addr));
	int thread_num = argc - 5;
	int waitus = calculate_pause (pps, frame_size, burst_size, thread_num, 1000);
	fprintf(stderr, "pps %i frame size %i burst %i line rate %i Mbps pause %i us\n",
		pps, frame_size, burst_size, 1000, waitus);

	int t;
	for (t = 0; t < thread_num; t++) {
		const char *target_addr_str = argv[t+5];
		parse_addr(&target_addrs[t], target_addr_str);

		fprintf(stderr, "[*] Sending to %s, send buffer %i packets\n",
			addr_to_str(&target_addrs[t]), packets_in_buf);
	}

	struct state *array_of_states = calloc(thread_num, sizeof(struct state));
	struct thread **array_of_threads = calloc(thread_num, sizeof(struct thread *));

	for (t = 0; t < thread_num; t++) {
		struct state *state = &array_of_states[t];
		state->target_addr = &target_addrs[t];
		state->packets_in_buf = packets_in_buf;
		state->payload = payload;
		state->payload_sz = payload_sz;
		state->src_port = 11404;
		state->cnt = loop_count;
		state->packets = 0;
		state->bytes = 0;
		state->cpu_id = t;
		state->waitus = waitus;
		array_of_threads[t] = thread_spawn(thread_loop, state, 1, state->cpu_id);
	}
	for (t = 0; t < thread_num; t++) {
		struct thread *thread = array_of_threads[t];
		thread_join(thread);
	}
	uint64_t total_packets = 0;
	uint64_t total_bytes = 0;
	for (t = 0; t < thread_num; t++) {
		struct state *state = &array_of_states[t];
		total_packets += state->packets;
		total_bytes += state->bytes;
	}
/*
	int cnt = loop_count;
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
		// pass
	}
*/
	fprintf(stderr, "Sent %lu packets  %lu bytes\n", total_packets, total_bytes);
	return 0;
}
