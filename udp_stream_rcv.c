#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h> 
#include <netdb.h>
#include <sys/time.h>
#include "udp_stream_common.h"

static void Usage(int argc, char *argv[]);
static void Print_help();

static const struct timeval Report_Interval = {5, 0};

static int Port;

int main(int argc, char *argv[]) {
    int sr;                  /* socket for receiving */
    struct sockaddr_in name; /* address to bind to receive */
    fd_set read_mask, tmp_mask;
    int rcvd_bytes;
    struct stream_pkt rcv_pkt;
    struct timeval start_ts, now;
    long int duration;
    double rate;
    int rcvd_count;
    int last_rcvd;
    int out_of_order_count;
    int num;
    double max_oneway;
    double oneway;
    struct timeval next_report_time;
    struct timeval timeout;
    struct timeval *to_ptr;

    /* Initialize */
    Usage(argc, argv);
    printf("Successfully initialized with:\n");
    printf("\tPort = %d\n", Port);

    /* Set up receiving socket */
    sr = socket(AF_INET, SOCK_DGRAM, 0);
    if (sr < 0) {
        perror("error opening input socket");
        exit(1);
    } 

    name.sin_family = AF_INET; 
    name.sin_addr.s_addr = INADDR_ANY; 
    name.sin_port = htons(Port);

    int flag = 1;  
    if (-1 == setsockopt(sr, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag))) {  
        perror("setsockopt fail");  
    }  

    if (bind(sr, (struct sockaddr *)&name, sizeof(name)) < 0) { 
        perror("bind error");
        exit(1);
    }

    /* Set up masks to register sockets for select */
    FD_ZERO(&read_mask);
    FD_ZERO(&tmp_mask);
    FD_SET(sr, &read_mask); /* we want select to return when there is something
                               to read on sr */

    /* Init counts / reporting data */
    rcvd_count = 0;
    out_of_order_count = 0;
    last_rcvd = 0;
    max_oneway = 0;

    /* Begin event loop */
    for (;;)
    {
        /* reset mask */
        tmp_mask = read_mask;
        /* reset timeout. only need to report if we've actually received
         * something already */
        if (rcvd_count == 0) {
            to_ptr = NULL;
        } else {
            gettimeofday(&now, NULL);
            timersub(&next_report_time, &now, &timeout);
            to_ptr = &timeout;
        }

        num = select(FD_SETSIZE, &tmp_mask, NULL, NULL, to_ptr);
        if (num > 0) {
            if (FD_ISSET(sr, &tmp_mask)) {
                /* Receive data */
                int from_len = sizeof(name);
                rcvd_bytes = recvfrom(sr, &rcv_pkt, sizeof(rcv_pkt), 0, (struct sockaddr*)&name, &from_len);
                if (rcvd_bytes <= 0) {
                    printf("error receiving\n");
                    exit(1);
                }

                //printf("received at port %d seq %d\n",Port, rcv_pkt.seq);

                /* If first packet, start timing */
                if (rcvd_count == 0) {
                    gettimeofday(&start_ts, NULL);
                    timeradd(&start_ts, &Report_Interval, &next_report_time);
                }

                /* Update counts */
                rcvd_count++;
                if (rcv_pkt.seq != last_rcvd + 1) {
                    if(rcv_pkt.seq < last_rcvd + 1) {
                        printf("warning: received seq %d, expected seq %d\n", rcv_pkt.seq, last_rcvd+1);
                    }
                    out_of_order_count++;
                }
                last_rcvd = rcv_pkt.seq;

                /* Calculate oneway delay */
                gettimeofday(&now, NULL);
                oneway = now.tv_sec - rcv_pkt.ts_sec;
                oneway *= 1000;
                oneway += (now.tv_usec - rcv_pkt.ts_usec) / 1000.0;
                if (oneway > max_oneway || rcvd_count == 1) {
                    max_oneway = oneway;
                }

            }
        } else { /* timeout */
            /* calculate current rate */
            gettimeofday(&now, NULL);
            duration = now.tv_sec - start_ts.tv_sec;
            duration *= 1000000;
            duration += now.tv_usec - start_ts.tv_usec;
            rate = MAX_DATA_LEN * rcv_pkt.seq * 8; /* bits sent so far */
            rate = rate / duration; /* bits per usec == megabits per sec */

            /* report */
            printf("%lf sec elapsed, avg rate %lf Mbps\n",
                   duration / 1000000.0, rate);
            printf("%d total pkts recvd, %d pkts out of order, last "
                   "recvd seq = %d\n, loss rate: %f\n", rcvd_count, out_of_order_count,
                   last_rcvd, out_of_order_count*1.0 / rcvd_count);
            printf("%lf ms max oneway delay\n\n", max_oneway);
            timeradd(&next_report_time, &Report_Interval, &next_report_time);
        }
    }

    return 0;
}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {
    if (argc != 2) {
        Print_help();
    }

    if (sscanf(argv[1], "%d", &Port) != 1) {
        Print_help();
    }
}

static void Print_help() {
    printf("Usage: udp_stream_rcv <port>\n");
    exit(0);
}