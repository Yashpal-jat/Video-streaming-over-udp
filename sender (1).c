#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>

#define SERVER_IP "127.0.0.1"
#define PORT 8888
#define PACKET_SIZE 1400
#define HEADER_SIZE 8

// AIMD Parameters -- unchanged from the original sender.c. AIMD still owns
// pacing (how fast we push bytes at the CURRENTLY SELECTED quality); ABS
// just decides WHICH quality's bytes those are. The two are complementary,
// not competing: AIMD reacts every ~50 packets, ABS only reacts at segment
// boundaries, so ABS is the coarse-grained decision and AIMD is the
// fine-grained one.
#define ALPHA 10
#define BETA 0.5
#define MIN_RATE 10
#define MAX_RATE 5000

#define ARTIFICIAL_LOSS_PROB 0.001

// ---- ABS additions ----
#define NUM_TIERS 3

typedef struct {
    const char *dir;
    unsigned int bitrate_bps;   // nominal encoded bitrate, used for tier decisions
    const char *label;
} Tier;

// Must match the bitrates baked into encode_renditions.sh
static Tier tiers[NUM_TIERS] = {
    { "renditions/low",  400000,  "LOW (400kbps/240p)"  },
    { "renditions/mid",  1200000, "MID (1200kbps/480p)" },
    { "renditions/high", 2500000, "HIGH(2500kbps/720p)" }
};

typedef struct {
    unsigned int seq_num;
    unsigned int timestamp;
    unsigned char data[PACKET_SIZE];
} Packet;

unsigned long get_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

// Decide which tier to use for the NEXT segment, given the AIMD rate
// estimate. Only ever steps one tier up or down per call (per segment
// boundary) -- real ABR players do the same, since jumping straight from
// 240p to 720p the moment bandwidth spikes tends to just cause another
// drop a few seconds later. Up/down thresholds are deliberately different
// (1.5x vs 1.1x headroom) so the choice doesn't flap back and forth when
// throughput is hovering right at a boundary -- this hysteresis gap is
// the main knob you'd tune if you were demoing this.
int choose_tier(int current_tier, unsigned int rate_pps) {
    double estimated_bps = (double)rate_pps * PACKET_SIZE * 8.0;

    if (current_tier < NUM_TIERS - 1) {
        double next_bitrate = tiers[current_tier + 1].bitrate_bps;
        if (estimated_bps > next_bitrate * 1.5) {
            return current_tier + 1;
        }
    }
    if (current_tier > 0) {
        double this_bitrate = tiers[current_tier].bitrate_bps;
        if (estimated_bps < this_bitrate * 1.1) {
            return current_tier - 1;
        }
    }
    return current_tier;
}

FILE *open_segment(int tier, int segment_idx) {
    char path[256];
    snprintf(path, sizeof(path), "%s/seg_%03d.h264", tiers[tier].dir, segment_idx);
    return fopen(path, "rb");
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    // No longer takes a single file on the command line -- segments live
    // under renditions/<tier>/seg_NNN.h264 (see encode_renditions.sh).
    // Run this from the directory that contains the "renditions" folder.

    FILE *log_fp = fopen("aimd_log.csv", "w");
    if (!log_fp) {
        perror("Could not open log file");
        return 1;
    }
    fprintf(log_fp, "TimeMS,Rate,Loss,Quality\n");
    unsigned long start_time = get_time_us();

    int sockfd;
    struct sockaddr_in receiver_addr, src_addr;
    socklen_t addr_len = sizeof(src_addr);

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        return 1;
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    memset(&receiver_addr, 0, sizeof(receiver_addr));
    receiver_addr.sin_family = AF_INET;
    receiver_addr.sin_port = htons(PORT);
    receiver_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    unsigned int current_rate = 500;
    unsigned int seq_num = 0;
    Packet pkt;
    int bytes_read;

    int current_tier = 0;   // start conservative (LOW) and let AIMD probe up,
                             // same "start safe" logic as TCP slow start
    int segment_idx = 0;

    printf("[Sender-ABS] Starting stream with %d quality tiers...\n", NUM_TIERS);

    char feedback_buf[128];
    float reported_loss = 0.0;

    FILE *fp = open_segment(current_tier, segment_idx);
    if (!fp) {
        fprintf(stderr,
            "Could not open %s/seg_%03d.h264 -- run ./encode_renditions.sh "
            "<video> first, and run this binary from the same directory.\n",
            tiers[current_tier].dir, segment_idx);
        fclose(log_fp);
        return 1;
    }

    while (1) {
        bytes_read = fread(pkt.data, 1, PACKET_SIZE, fp);

        if (bytes_read <= 0) {
            // Current segment exhausted. This is the ONLY point where we
            // allow a quality switch, because every segment starts on a
            // keyframe (guaranteed by encode_renditions.sh) -- so jumping
            // to a different tier here never lands mid-GOP.
            fclose(fp);
            segment_idx++;

            int new_tier = choose_tier(current_tier, current_rate);
            if (new_tier != current_tier) {
                fprintf(stderr, "\n[ABS] Switching quality: %s -> %s (at segment %d, ~%.1f Mbps estimated)\n",
                        tiers[current_tier].label, tiers[new_tier].label, segment_idx,
                        (current_rate * PACKET_SIZE * 8.0) / 1e6);
                current_tier = new_tier;
            }

            fp = open_segment(current_tier, segment_idx);
            if (!fp) {
                break; // ran out of segments at this tier -- stream complete
            }
            continue;
        }

        pkt.seq_num = htonl(seq_num++);
        pkt.timestamp = htonl((unsigned int)(get_time_us() / 1000));

        double random_val = (double)rand() / RAND_MAX;
        if (random_val > ARTIFICIAL_LOSS_PROB) {
            sendto(sockfd, &pkt, bytes_read + HEADER_SIZE, 0,
                   (const struct sockaddr *)&receiver_addr, sizeof(receiver_addr));
        }

        if (current_rate < MIN_RATE) current_rate = MIN_RATE;
        unsigned int sleep_us = 1000000 / current_rate;
        usleep(sleep_us);

        int n = recvfrom(sockfd, feedback_buf, sizeof(feedback_buf) - 1, 0,
                          (struct sockaddr *)&src_addr, &addr_len);

        if (n > 0) {
            feedback_buf[n] = '\0';
            if (strncmp(feedback_buf, "LOSS", 4) == 0) {
                sscanf(feedback_buf, "LOSS %f", &reported_loss);

                if (reported_loss > 0.05) {
                    current_rate = (unsigned int)(current_rate * BETA);
                    if (current_rate < MIN_RATE) current_rate = MIN_RATE;
                    fprintf(stderr, "\n[AIMD] Loss: %.2f%% | Rate: %d (DROP) | Quality: %s",
                            reported_loss * 100, current_rate, tiers[current_tier].label);
                } else {
                    current_rate += ALPHA;
                    if (current_rate > MAX_RATE) current_rate = MAX_RATE;
                    if (seq_num % 50 == 0)
                        fprintf(stderr, "\r[AIMD] Loss: %.2f%% | Rate: %d (RISE) | Quality: %s",
                                reported_loss * 100, current_rate, tiers[current_tier].label);
                }

                unsigned long relative_time = (get_time_us() - start_time) / 1000;
                fprintf(log_fp, "%lu,%d,%.4f,%s\n", relative_time, current_rate,
                        reported_loss, tiers[current_tier].label);
            }
        }
    }

    printf("\n[Sender-ABS] Done. Data saved to 'aimd_log.csv'.\n");
    fclose(log_fp);
    close(sockfd);
    return 0;
}
