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

#ifndef CS2520_EX2_UDP_STREAM
#define CS2520_EX2_UDP_STREAM

#define TARGET_RATE 8 /* rate to send at, in Mbps */
#define MAX_DATA_LEN 1300
#define REPORT_SEC 5 /* print status every REPORT_SEC seconds */

struct stream_pkt {
    int32_t seq;
    int32_t ts_sec;
    int32_t ts_usec;
    int32_t size;
    char data[MAX_DATA_LEN];
};

struct ack_pkt {
    int32_t ack_no;
    struct timeval ts;
};

struct rt_packet{
    int32_t seq;
    struct timeval ts;
    int32_t size;
    char data[MAX_DATA_LEN];
};

#endif
