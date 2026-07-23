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

#define RELAY_IN_PORT         47002
#define HARNESS_OUT_PORT      47020
#define HARNESS_IP            "127.0.0.1"

#define PAYLOAD_SIZE          160
#define SEQ_SIZE              4
#define FRAME_SIZE            (SEQ_SIZE + PAYLOAD_SIZE)   /* 164 bytes */
#define BUFFER_SIZE           8192
#define FEC_FLAG              0x80000000u
#define TOTAL_FRAMES          1500
#define FRAME_INTERVAL_NS     20000000LL                  /* 20ms */

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

/* Debug counters */
static int debug_fec_recovered_count = 0;
static int debug_concealed_count = 0;

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

    pthread_mutex_lock(&anchor_mutex);
    while (!anchor_ready) {
        pthread_cond_wait(&anchor_cond, &anchor_mutex);
    }
    struct timespec local_anchor = anchor_time;
    uint32_t local_first_seq = first_seq;
    pthread_mutex_unlock(&anchor_mutex);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* 22ms buffer offset allows FEC packets arriving at +50ms to repair lost even frames */
    long playout_offset_ms = 22;

    fprintf(stderr, "\n[DEBUG] === Playout Thread Started ===\n");
    fprintf(stderr, "[DEBUG] Applied Buffer Offset: %ld ms\n", playout_offset_ms);
    fprintf(stderr, "[DEBUG] Anchor First Packet Seq: %u\n", local_first_seq);
    fprintf(stderr, "[DEBUG] Anchor Recv Time: %ld.%09ld s\n", 
            (long)local_anchor.tv_sec, local_anchor.tv_nsec);

    struct timespec start_time = local_anchor;
    timespec_add_ns(&start_time, playout_offset_ms * 1000000LL);
    timespec_add_ns(&start_time, -((long long)local_first_seq * FRAME_INTERVAL_NS));

    long long start_diff_ms = ((long long)(start_time.tv_sec - now.tv_sec) * 1000LL) +
                              ((start_time.tv_nsec - now.tv_nsec) / 1000000LL);

    fprintf(stderr, "[DEBUG] Computed Start Time (Frame 0): %ld.%09ld s\n", 
            (long)start_time.tv_sec, start_time.tv_nsec);
    fprintf(stderr, "[DEBUG] Time until Frame 0 playout: %lld ms\n", start_diff_ms);

    for (uint32_t current_seq = 0; current_seq < TOTAL_FRAMES; current_seq++) {
        struct timespec target = start_time;
        timespec_add_ns(&target, (long long)current_seq * FRAME_INTERVAL_NS);

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL);

        struct timespec wakeup_time;
        clock_gettime(CLOCK_MONOTONIC, &wakeup_time);

        uint8_t send_buf[FRAME_SIZE];
        uint32_t net_seq = htonl(current_seq);
        memcpy(send_buf, &net_seq, SEQ_SIZE);

        pthread_mutex_lock(&buf_mutex);
        int idx = current_seq % BUFFER_SIZE;
        int was_received = jitter_buf[idx].received;
        if (was_received) {
            memcpy(send_buf + SEQ_SIZE, jitter_buf[idx].payload, PAYLOAD_SIZE);
        } else {
            memset(send_buf + SEQ_SIZE, 0, PAYLOAD_SIZE); // Concealment
            debug_concealed_count++;
            fprintf(stderr, "[DEBUG CONCEALED] Frame %u played as SILENCE (missed/unrecovered)\n", current_seq);
        }
        pthread_mutex_unlock(&buf_mutex);

        if (current_seq == 0 || current_seq == 1 || current_seq == 10 || 
            current_seq == 100 || current_seq == 750 || current_seq == 1499) {
            long long delta_us = ((long long)(wakeup_time.tv_sec - target.tv_sec) * 1000000LL) +
                                 ((wakeup_time.tv_nsec - target.tv_nsec) / 1000LL);
            fprintf(stderr, "[DEBUG] Frame %4u | Sent at: %ld.%09ld | Delta: %lld us | Status: %s\n",
                    current_seq, (long)wakeup_time.tv_sec, wakeup_time.tv_nsec, 
                    delta_us, was_received ? "OK" : "CONCEALED");
        }

        sendto(out_fd, send_buf, sizeof(send_buf), 0,
               (struct sockaddr *)&harness_player, sizeof(harness_player));
    }
    
    fprintf(stderr, "[DEBUG] === Playout Completed All %d Frames ===\n", TOTAL_FRAMES);
    fprintf(stderr, "[DEBUG] Total Frames Recovered by FEC: %d\n", debug_fec_recovered_count);
    fprintf(stderr, "[DEBUG] Total Frames Played as Concealment: %d\n\n", debug_concealed_count);
    return NULL;
}

static void record_anchor_if_first(uint32_t seq) {
    pthread_mutex_lock(&anchor_mutex);
    if (!anchor_ready) {
        clock_gettime(CLOCK_MONOTONIC, &anchor_time);
        first_seq = seq;
        anchor_ready = 1;
        fprintf(stderr, "[DEBUG] First packet received! Seq: %u\n", seq);
        pthread_cond_signal(&anchor_cond);
    }
    pthread_mutex_unlock(&anchor_mutex);
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (in_fd < 0) {
        perror("socket (in)");
        exit(EXIT_FAILURE);
    }

    int reuse = 1;
    setsockopt(in_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in in_addr;
    memset(&in_addr, 0, sizeof(in_addr));
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(RELAY_IN_PORT);
    in_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) {
        perror("bind");
        close(in_fd);
        exit(EXIT_FAILURE);
    }

    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (out_fd < 0) {
        perror("socket (out)");
        close(in_fd);
        exit(EXIT_FAILURE);
    }

    memset(&harness_player, 0, sizeof(harness_player));
    harness_player.sin_family = AF_INET;
    harness_player.sin_port = htons(HARNESS_OUT_PORT);
    inet_pton(AF_INET, HARNESS_IP, &harness_player.sin_addr);

    pthread_t thread_id;
    pthread_create(&thread_id, NULL, playout_thread, NULL);

    uint8_t wire_buf[FRAME_SIZE];
    for (;;) {
        ssize_t n = recvfrom(in_fd, wire_buf, sizeof(wire_buf), 0, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            continue;
        }
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
                    debug_fec_recovered_count++;
                    fprintf(stderr, "[DEBUG FEC RECOVERY] Recovered Frame %u\n", seq + 1);
                }
            } else {
                int fec_idx = seq % BUFFER_SIZE;
                int prev_idx = (seq + BUFFER_SIZE - 1) % BUFFER_SIZE;
                if (fec_buf[fec_idx].received && !jitter_buf[prev_idx].received) {
                    for (int i = 0; i < PAYLOAD_SIZE; i++) {
                        jitter_buf[prev_idx].payload[i] =
                            wire_buf[SEQ_SIZE + i] ^ fec_buf[fec_idx].payload[i];
                    }
                    jitter_buf[prev_idx].received = 1;
                    debug_fec_recovered_count++;
                    fprintf(stderr, "[DEBUG FEC RECOVERY] Recovered Frame %u\n", seq - 1);
                }
            }
        } else {
            int idx = seq % BUFFER_SIZE;
            fec_buf[idx].received = 1;
            memcpy(fec_buf[idx].payload, wire_buf + SEQ_SIZE, PAYLOAD_SIZE);

            int curr_idx = seq % BUFFER_SIZE;
            int prev_idx = (seq + BUFFER_SIZE - 1) % BUFFER_SIZE;

            if (jitter_buf[curr_idx].received && !jitter_buf[prev_idx].received) {
                for (int i = 0; i < PAYLOAD_SIZE; i++) {
                    jitter_buf[prev_idx].payload[i] =
                        wire_buf[SEQ_SIZE + i] ^ jitter_buf[curr_idx].payload[i];
                }
                jitter_buf[prev_idx].received = 1;
                debug_fec_recovered_count++;
                fprintf(stderr, "[DEBUG FEC RECOVERY] Recovered Frame %u via FEC %u\n", seq - 1, seq);
            } else if (jitter_buf[prev_idx].received && !jitter_buf[curr_idx].received) {
                for (int i = 0; i < PAYLOAD_SIZE; i++) {
                    jitter_buf[curr_idx].payload[i] =
                        wire_buf[SEQ_SIZE + i] ^ jitter_buf[prev_idx].payload[i];
                }
                jitter_buf[curr_idx].received = 1;
                debug_fec_recovered_count++;
                fprintf(stderr, "[DEBUG FEC RECOVERY] Recovered Frame %u via FEC %u\n", seq, seq);
            }
        }

        pthread_mutex_unlock(&buf_mutex);
    }

    close(in_fd);
    close(out_fd);
    return 0;
}