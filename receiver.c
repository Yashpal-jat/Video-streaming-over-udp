#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

#define PORT 8888
#define BUFFER_SIZE 2048
#define WINDOW_SIZE 50          
#define PACKET_DATA_MAX 1400   


#define REORDER_WINDOW 32      
#define REORDER_TIMEOUT_MS 40   
typedef struct {
    int valid;
    unsigned int seq_num;
    int data_len;
    unsigned char data[PACKET_DATA_MAX];
} Slot;

static Slot window_buf[REORDER_WINDOW];

unsigned long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int main(void) {
    int sockfd;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

   
    int rcvbuf_size = 1024 * 1024;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) < 0) {
        perror("Could not set socket buffer size");
    }

    
    struct timeval rcvtimeo = { .tv_sec = 0, .tv_usec = 10000 }; // 10ms
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

    struct sockaddr_in serv_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    memset(&serv_addr, 0, sizeof(serv_addr));
    memset(&client_addr, 0, sizeof(client_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT);

    if (bind(sockfd, (const struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    char buffer[BUFFER_SIZE];

    unsigned int next_seq_to_release = 0;
    unsigned long gap_start_ms = 0;    // 0 = no active gap currently being timed
    unsigned int highest_seq_seen = 0; // highest seq_num we've actually stored
    int have_seen_any = 0;             // distinguishes "highest=0 for real" from "nothing yet"

    int lost_count = 0;
    int total_packets_window = 0;

    fprintf(stderr, "[Receiver] Listening on port %d (reorder-tolerant)...\n", PORT);
    fprintf(stderr, "[Receiver] Pipe this output to ffplay!\n");

    while (1) {
        int len = recvfrom(sockfd, buffer, BUFFER_SIZE, 0,
                            (struct sockaddr *)&client_addr, &addr_len);

        if (len > 0) {
            unsigned int seq_num, timestamp;
            memcpy(&seq_num, buffer, 4);
            memcpy(&timestamp, buffer + 4, 4);
            seq_num = ntohl(seq_num);
            timestamp = ntohl(timestamp);
            (void)timestamp; 

            int data_len = len - 8;

            if (seq_num < next_seq_to_release) {
                // Arrived so late we already gave up on it (or it's a
                // duplicate). Using it now would corrupt playback order,
                // so it's dropped -- this is the one case we can't recover.
            } else if (data_len > 0 && data_len <= PACKET_DATA_MAX) {
                unsigned int idx = seq_num % REORDER_WINDOW;
                if (window_buf[idx].valid && window_buf[idx].seq_num != seq_num) {
                    
                    
                    fprintf(stderr, "\n[Reorder] window collision at seq %u -- dropping\n", seq_num);
                } else {
                    window_buf[idx].valid = 1;
                    window_buf[idx].seq_num = seq_num;
                    window_buf[idx].data_len = data_len;
                    memcpy(window_buf[idx].data, buffer + 8, data_len);
                    if (!have_seen_any || seq_num > highest_seq_seen) {
                        highest_seq_seen = seq_num;
                        have_seen_any = 1;
                    }
                }
            }
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("recvfrom");
        }
       
      
        int made_progress = 1;
        while (made_progress) {
            made_progress = 0;
            unsigned int idx = next_seq_to_release % REORDER_WINDOW;

            if (window_buf[idx].valid && window_buf[idx].seq_num == next_seq_to_release) {
                fwrite(window_buf[idx].data, 1, window_buf[idx].data_len, stdout);
                fflush(stdout);
                window_buf[idx].valid = 0;
                next_seq_to_release++;
                gap_start_ms = 0;
                total_packets_window++;
                made_progress = 1;
            } else if (have_seen_any && highest_seq_seen > next_seq_to_release) {
                unsigned long t = now_ms();
                if (gap_start_ms == 0) {
                    gap_start_ms = t;
                } else if (t - gap_start_ms > REORDER_TIMEOUT_MS) {
                    fprintf(stderr, "\n[Reorder] Gave up waiting for seq %u -- marking lost\n",
                            next_seq_to_release);
                    lost_count++;
                    total_packets_window++;
                    next_seq_to_release++;
                    gap_start_ms = 0;
                    made_progress = 1; // retry: may cascade-release what was queued behind it
                }
            }
            
        }
        if (total_packets_window >= WINDOW_SIZE) {
            float loss_rate = (float)lost_count / total_packets_window;

            char feedback_msg[64];
            snprintf(feedback_msg, sizeof(feedback_msg), "LOSS %f", loss_rate);
            sendto(sockfd, feedback_msg, strlen(feedback_msg), 0,
                   (const struct sockaddr *)&client_addr, addr_len);

            lost_count = 0;
            total_packets_window = 0;
        }
    }

    close(sockfd);
    return 0;
}
