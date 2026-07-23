/*
 * sender.c - Low-latency real-time UDP audio streaming sender with 2:1 XOR FEC
 *
 * Pipeline:
 *   Harness --(UDP :47010)--> sender.c --(UDP :47001)--> relay --> receiver
 *
 * Input frame format (from harness on port 47010), exactly 164 bytes:
 *   bytes [0..3]   uint32_t seq     (big-endian / network byte order)
 *   bytes [4..163] uint8_t  payload[160]  (raw PCM audio)
 *
 * Output to relay on port 47001:
 *   1. Primary packet: the 164-byte input frame, forwarded unchanged,
 *      immediately, with no buffering delay.
 *   2. FEC packet, emitted only when the incoming seq N is odd (N % 2 == 1):
 *        seq (network order)  = N | 0x80000000   (high bit marks "this is FEC")
 *        payload              = payload[N-1] XOR payload[N]
 *      A receiver that is missing exactly one of the pair (N-1, N) can XOR
 *      the surviving primary packet against this FEC packet to reconstruct
 *      the missing one.
 *
 * Bandwidth accounting (1500 input frames, seq 0..1499):
 *   1500 primary packets + 750 FEC packets (one per odd seq) = 2250 packets
 *   2250 * 164 bytes = 369,000 bytes total
 *   369,000 / 240,000 (raw 1500*160 payload baseline) = 1.5375x overhead
 *   -> comfortably under the 2.00x hard cap, in line with the ~1.5x target.
 *
 * Every even frame N is protected by the FEC packet sent for N+1, and every
 * odd frame N is protected by the FEC packet sent for itself -- so all 1500
 * frames are covered by pairwise XOR redundancy with no extra packets beyond
 * one per odd sequence number.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define HARNESS_IN_PORT 47010
#define RELAY_OUT_PORT  47001
#define RELAY_IP        "127.0.0.1"

#define PAYLOAD_SIZE    160
#define SEQ_SIZE        4
#define FRAME_SIZE      (SEQ_SIZE + PAYLOAD_SIZE)   /* 164 bytes */
#define FEC_FLAG        0x80000000u

/* Create a UDP socket bound to the given port on all local interfaces. */
static int make_bound_socket(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket (in)");
        exit(EXIT_FAILURE);
    }

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        /* Non-fatal: continue even if this fails. */
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        exit(EXIT_FAILURE);
    }
    return fd;
}

int main(void) {
    /* Socket for receiving raw frames from the local test harness. */
    int in_fd = make_bound_socket(HARNESS_IN_PORT);

    /* Socket for forwarding primary + FEC packets to the relay. */
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (out_fd < 0) {
        perror("socket (out)");
        close(in_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in relay_addr;
    memset(&relay_addr, 0, sizeof(relay_addr));
    relay_addr.sin_family = AF_INET;
    relay_addr.sin_port = htons(RELAY_OUT_PORT);
    if (inet_pton(AF_INET, RELAY_IP, &relay_addr.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed for %s\n", RELAY_IP);
        close(in_fd);
        close(out_fd);
        exit(EXIT_FAILURE);
    }

    uint8_t in_buf[FRAME_SIZE];
    uint8_t prev_payload[PAYLOAD_SIZE];
    uint32_t prev_seq = 0;
    int have_prev = 0;

    for (;;) {
        ssize_t n = recvfrom(in_fd, in_buf, sizeof(in_buf), 0, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            continue;
        }
        if (n != (ssize_t)FRAME_SIZE) {
            /* Malformed/short frame -- drop it and keep the pipeline moving. */
            continue;
        }

        uint32_t seq_net;
        memcpy(&seq_net, in_buf, SEQ_SIZE);
        uint32_t seq = ntohl(seq_net);
        const uint8_t *payload = in_buf + SEQ_SIZE;

        /* 1. Forward the primary packet immediately, byte-for-byte. */
        if (sendto(out_fd, in_buf, FRAME_SIZE, 0,
                   (struct sockaddr *)&relay_addr, sizeof(relay_addr)) < 0) {
            perror("sendto (primary)");
        }

        /* 2. On every odd seq, emit one XOR-FEC packet covering (seq-1, seq). */
        if ((seq % 2u) == 1u && have_prev && prev_seq == seq - 1u) {
            uint8_t fec_buf[FRAME_SIZE];
            uint32_t fec_seq_net = htonl(seq | FEC_FLAG);
            memcpy(fec_buf, &fec_seq_net, SEQ_SIZE);

            for (int i = 0; i < PAYLOAD_SIZE; i++) {
                fec_buf[SEQ_SIZE + i] = prev_payload[i] ^ payload[i];
            }

            if (sendto(out_fd, fec_buf, FRAME_SIZE, 0,
                       (struct sockaddr *)&relay_addr, sizeof(relay_addr)) < 0) {
                perror("sendto (fec)");
            }
        }

        memcpy(prev_payload, payload, PAYLOAD_SIZE);
        prev_seq = seq;
        have_prev = 1;
    }

    close(in_fd);
    close(out_fd);
    return 0;
}