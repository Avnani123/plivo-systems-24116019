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
static pthread_cond_t anchor_cond;
static int anchor_ready = 0;
static struct timespec min_e0;

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

static int timespec_cmp(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec < b->tv_sec) return -1;
    if (a->tv_sec > b->tv_sec) return 1;
    if (a->tv_nsec < b->tv_nsec) return -1;
    if (a->tv_nsec > b->tv_nsec) return 1;
    return 0;
}

static void *playout_thread(void *arg) {
    (void)arg;

    pthread_mutex_lock(&anchor_mutex);
    while (!anchor_ready) {
        pthread_cond_wait(&anchor_cond, &anchor_mutex);
    }

    /* 8ms offset keeps total playout delay at ~48ms (safely below 60ms cap) */
    long playout_offset_ms = 8;
    struct timespec frame0_target;

    while (1) {
        frame0_target = min_e0;
        timespec_add_ns(&frame0_target, playout_offset_ms * 1000000LL);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        if (timespec_cmp(&now, &frame0_target) >= 0) {
            break;
        }

        int rc = pthread_cond_timedwait(&anchor_cond, &anchor_mutex, &frame0_target);
        if (rc == ETIMEDOUT) {
            break;
        }
    }

    /* Freeze the timeline anchor for playout */
    struct timespec start_time = frame0_target;
    pthread_mutex_unlock(&anchor_mutex);

    fprintf(stderr, "\n[DEBUG] === Playout Thread Started ===\n");
    fprintf(stderr, "[DEBUG] Frozen Playout Start Time (Frame 0): %ld.%09ld s\n", 
            (long)start_time.tv_sec, start_time.tv_nsec);

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
        }
        pthread_mutex_unlock(&buf_mutex);

        if (current_seq == 0 || current_seq == 1 || current_seq == 10 || 
            current_seq == 100 || current_seq == 750 || current_seq == 1499) {
            fprintf(stderr, "[DEBUG] Frame %4u | Sent at: %ld.%09ld | Status: %s\n",
                    current_seq, (long)wakeup_time.tv_sec, wakeup_time.tv_nsec, 
                    was_received ? "OK" : "CONCEALED");
        }

        sendto(out_fd, send_buf, sizeof(send_buf), 0,
               (struct sockaddr *)&harness_player, sizeof(harness_player));
    }

    fprintf(stderr, "[DEBUG] === Playout Completed All %d Frames ===\n", TOTAL_FRAMES);
    fprintf(stderr, "[DEBUG] Total Frames Recovered by FEC: %d\n", debug_fec_recovered_count);
    fprintf(stderr, "[DEBUG] Total Frames Played as Concealment: %d\n\n", debug_concealed_count);
    return NULL;
}

static void update_anchor(uint32_t seq, const struct timespec *recv_time) {
    struct timespec e0 = *recv_time;
    timespec_add_ns(&e0, -((long long)seq * FRAME_INTERVAL_NS));

    pthread_mutex_lock(&anchor_mutex);
    if (!anchor_ready) {
        min_e0 = e0;
        anchor_ready = 1;
        pthread_cond_signal(&anchor_cond);
    } else if (timespec_cmp(&e0, &min_e0) < 0) {
        min_e0 = e0;
        pthread_cond_signal(&anchor_cond);
    }
    pthread_mutex_unlock(&anchor_mutex);
}

int main(void) {
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&anchor_cond, &cattr);
    pthread_condattr_destroy(&cattr);

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

        struct timespec recv_time;
        clock_gettime(CLOCK_MONOTONIC, &recv_time);

        uint32_t net_seq;
        memcpy(&net_seq, wire_buf, SEQ_SIZE);
        uint32_t raw_seq = ntohl(net_seq);

        int is_fec = (raw_seq & FEC_FLAG) != 0;
        uint32_t seq = raw_seq & ~FEC_FLAG;

        update_anchor(seq, &recv_time);

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
            } else if (jitter_buf[prev_idx].received && !jitter_buf[curr_idx].received) {
                for (int i = 0; i < PAYLOAD_SIZE; i++) {
                    jitter_buf[curr_idx].payload[i] =
                        wire_buf[SEQ_SIZE + i] ^ jitter_buf[prev_idx].payload[i];
                }
                jitter_buf[curr_idx].received = 1;
                debug_fec_recovered_count++;
            }
        }

        pthread_mutex_unlock(&buf_mutex);
    }

    close(in_fd);
    close(out_fd);
    return 0;
}