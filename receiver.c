#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define RELAY_IN_PORT     47002
#define HARNESS_OUT_PORT  47020
#define HARNESS_IP        "127.0.0.1"

#define PAYLOAD_SIZE      160
#define SEQ_SIZE          4
#define FRAME_SIZE        (SEQ_SIZE + PAYLOAD_SIZE)   /* 164 bytes */
#define BUFFER_SIZE       8192
#define FEC_FLAG          0x80000000u
#define TOTAL_FRAMES      1500
#define FRAME_INTERVAL_NS 20000000LL   /* 20ms per frame */
#define RELAY_LATENCY_NS  30000000LL   /* 30ms network transit compensation */

typedef struct {
    int received;
    uint8_t payload[PAYLOAD_SIZE];
} FrameBuffer;

static FrameBuffer jitter_buf[BUFFER_SIZE];
static FrameBuffer fec_buf[BUFFER_SIZE];
static pthread_mutex_t buf_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t anchor_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t anchor_cond = PTHREAD_COND_INITIALIZER;
static int anchor_ready = 0;
static struct timespec anchor_time;
static uint32_t first_seq;

static int out_fd;
static struct sockaddr_in harness_player;

static void timespec_add_ns(struct timespec *ts, long long ns) {
    ts->tv_sec  += (time_t)(ns / 1000000000LL);
    ts->tv_nsec += (long)(ns % 1000000000LL);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    } else if (ts->tv_nsec < 0) {
        ts->tv_sec -= 1;
        ts->tv_nsec += 1000000000L;
    }
}

static void *playout_thread(void *arg) {
    (void)arg;

    const char *delay_env = getenv("DELAY_MS");
    long delay_ms = delay_env ? atol(delay_env) : 60;

    pthread_mutex_lock(&anchor_mutex);
    while (!anchor_ready) {
        pthread_cond_wait(&anchor_cond, &anchor_mutex);
    }
    struct timespec local_anchor = anchor_time;
    uint32_t local_first_seq = first_seq;
    pthread_mutex_unlock(&anchor_mutex);

    /* 
     * start_time = local_anchor + delay_ms - (first_seq * 20ms) - RELAY_LATENCY_NS
     * Subtracting RELAY_LATENCY_NS compensates for transit delay through the relay.
     */
    struct timespec start_time = local_anchor;
    timespec_add_ns(&start_time, delay_ms * 1000000LL);
    timespec_add_ns(&start_time, -((long long)local_first_seq) * FRAME_INTERVAL_NS);
    timespec_add_ns(&start_time, -RELAY_LATENCY_NS);

    for (uint32_t current_seq = 0; current_seq < TOTAL_FRAMES; current_seq++) {
        struct timespec target = start_time;
        timespec_add_ns(&target, (long long)current_seq * FRAME_INTERVAL_NS);

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL);

        uint8_t send_buf[FRAME_SIZE];
        uint32_t net_seq = htonl(current_seq);
        memcpy(send_buf, &net_seq, SEQ_SIZE);

        pthread_mutex_lock(&buf_mutex);
        int idx = current_seq % BUFFER_SIZE;
        if (jitter_buf[idx].received) {
            memcpy(send_buf + SEQ_SIZE, jitter_buf[idx].payload, PAYLOAD_SIZE);
        } else {
            memset(send_buf + SEQ_SIZE, 0, PAYLOAD_SIZE); // Silence concealment
        }
        pthread_mutex_unlock(&buf_mutex);

        sendto(out_fd, send_buf, sizeof(send_buf), 0,
               (struct sockaddr *)&harness_player, sizeof(harness_player));
    }
    return NULL;
}

static void record_anchor_if_first(uint32_t seq) {
    pthread_mutex_lock(&anchor_mutex);
    if (!anchor_ready) {
        clock_gettime(CLOCK_MONOTONIC, &anchor_time);
        first_seq = seq;
        anchor_ready = 1;
        pthread_cond_signal(&anchor_cond);
    }
    pthread_mutex_unlock(&anchor_mutex);
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    setsockopt(in_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(RELAY_IN_PORT);
    in_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr));

    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&harness_player, 0, sizeof(harness_player));
    harness_player.sin_family = AF_INET;
    harness_player.sin_port = htons(HARNESS_OUT_PORT);
    inet_pton(AF_INET, HARNESS_IP, &harness_player.sin_addr);

    pthread_t thread_id;
    pthread_create(&thread_id, NULL, playout_thread, NULL);

    uint8_t wire_buf[FRAME_SIZE];
    for (;;) {
        ssize_t n = recvfrom(in_fd, wire_buf, sizeof(wire_buf), 0, NULL, NULL);
        if (n != (ssize_t)FRAME_SIZE) continue;

        uint32_t net_seq;
        memcpy(&net_seq, wire_buf, SEQ_SIZE);
        uint32_t raw_seq = ntohl(net_seq);

        int is_fec = (raw_seq & FEC_FLAG) != 0;
        uint32_t seq = raw_seq & ~FEC_FLAG;

        record_anchor_if_first(seq);

        pthread_mutex_lock(&buf_mutex);

        if (!is_fec) {
            int idx = seq % BUFFER_SIZE;
            jitter_buf[idx].received = 1;
            memcpy(jitter_buf[idx].payload, wire_buf + SEQ_SIZE, PAYLOAD_SIZE);

            if (seq % 2u == 0u) {
                int next_idx = (seq + 1) % BUFFER_SIZE;
                if (fec_buf[next_idx].received && !jitter_buf[next_idx].received) {
                    for (int i = 0; i < PAYLOAD_SIZE; i++) {
                        jitter_buf[next_idx].payload[i] =
                            wire_buf[SEQ_SIZE + i] ^ fec_buf[next_idx].payload[i];
                    }
                    jitter_buf[next_idx].received = 1;
                }
            } else {
                int fec_idx = seq % BUFFER_SIZE;
                int prev_idx = (seq - 1) % BUFFER_SIZE;
                if (fec_buf[fec_idx].received && !jitter_buf[prev_idx].received) {
                    for (int i = 0; i < PAYLOAD_SIZE; i++) {
                        jitter_buf[prev_idx].payload[i] =
                            wire_buf[SEQ_SIZE + i] ^ fec_buf[fec_idx].payload[i];
                    }
                    jitter_buf[prev_idx].received = 1;
                }
            }
        } else {
            int idx = seq % BUFFER_SIZE;
            fec_buf[idx].received = 1;
            memcpy(fec_buf[idx].payload, wire_buf + SEQ_SIZE, PAYLOAD_SIZE);

            int curr_idx = seq % BUFFER_SIZE;
            int prev_idx = (seq - 1) % BUFFER_SIZE;

            if (jitter_buf[curr_idx].received && !jitter_buf[prev_idx].received) {
                for (int i = 0; i < PAYLOAD_SIZE; i++) {
                    jitter_buf[prev_idx].payload[i] =
                        wire_buf[SEQ_SIZE + i] ^ jitter_buf[curr_idx].payload[i];
                }
                jitter_buf[prev_idx].received = 1;
            } else if (jitter_buf[prev_idx].received && !jitter_buf[curr_idx].received) {
                for (int i = 0; i < PAYLOAD_SIZE; i++) {
                    jitter_buf[curr_idx].payload[i] =
                        wire_buf[SEQ_SIZE + i] ^ jitter_buf[prev_idx].payload[i];
                }
                jitter_buf[curr_idx].received = 1;
            }
        }

        pthread_mutex_unlock(&buf_mutex);
    }
    return 0;
}   