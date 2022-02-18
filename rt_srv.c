#include "sendto_dbg.h"
#include "udp_stream_common.h"
#include <sys/stat.h>
#include <unistd.h> 
#include <math.h>


static void Usage(int argc, char *argv[]);
static void Print_help();
static int Cmp_time(struct timeval t1, struct timeval t2);


static int WINDOW_SIZE = 1786;
static int loss_percent, in_port, out_port;

static int total_packets = 0, retransmissions = 0, highest_seq = 0;
struct timeval starts_at = {0,0};
#define max(a,b)            (((a) < (b)) ? (b) : (a))


socklen_t get_socket_len(struct sockaddr_in addr){
    return sizeof(addr);
}

void create_ack_pkt(struct ack_pkt *ack, int ack_no){
    ack->ack_no = ack_no;
    gettimeofday(&ack->ts, NULL);
}

void create_rt_pkt(struct rt_packet *packet, struct stream_pkt *strm_pkt, int seq_no){
    struct timeval now;
    packet->seq = seq_no;
    gettimeofday(&packet->ts, NULL);
    memcpy(&packet->data, &strm_pkt->data, sizeof(strm_pkt->data));
}

int is_empty(struct rt_packet buf, size_t size)
{
    struct rt_packet zero;
    memset(&zero, 0, sizeof(struct rt_packet));
    return !memcmp(&zero, &buf, sizeof(struct rt_packet));
}



int init_new_connection(int sock, int ip, struct sockaddr_in  addr, fd_set read_mask, int ack_no){
    struct ack_pkt ack;
    struct timeval now;
    socklen_t from_len;
    fd_set mask;
    for(;;){
        ack.ack_no = ack_no;
        gettimeofday(&now, NULL);
        memcpy(&ack.ts, &now, sizeof(now));
        /* send ack packet to client with the same ack_no*/
        sendto_dbg(sock, (char *)&ack, sizeof(ack), 0, 
            (struct sockaddr *)&addr, sizeof(addr));
        //printf("sending ack packet at port %d for seq %d\n", out_port, ack.ack_no);
        /* wait for client to reply back*/
        mask = read_mask;
        int num = select(FD_SETSIZE, &mask, NULL, NULL, NULL);
        if (num > 0) {
            if (FD_ISSET(sock, &mask)){
                from_len = sizeof(addr);
                int bytes = recvfrom(sock, &ack, sizeof(ack), 0,  
                          (struct sockaddr *)&addr, 
                          &from_len);
                int from_ip = addr.sin_addr.s_addr;
                if(ip != from_ip){
                    /* another client is trying to connect, server will reject both connections since we're not keeping a timeout for server*/
                    return 0;
                }else{
                    /* the server heard back from the client */
                    if(ack.ack_no < 0){ /*the previous ack set by the server is lost */
                        ack_no = ack.ack_no;
                        continue;
                    }else if(ack.ack_no == 0){
                        /* report success */
                        return 1;
                    }
                }
            }
        }
    }
    
}

double convert_time_to_usec(struct timeval t_time){
    long int t = t_time.tv_sec * 1000000;
    return t * 1.0 + t_time.tv_usec;
}


void print_stats(struct timeval cur_time){
    struct timeval diff_time;
    timersub(&cur_time, &starts_at, &diff_time);
    double in_usec = convert_time_to_usec(diff_time);
    double in_sec = in_usec / 1000000.0;
    printf("packets sent: %d, avg rate: %lf packets/sec, data_sent: %lf MB, avg rate: %lf Mbps, Retransmissions: %d, Highest Seq Sent: %d\n",
                                        total_packets, total_packets / in_sec, MAX_DATA_LEN * total_packets / 1000000.0,
                                        MAX_DATA_LEN * total_packets * 8.0 / in_usec, retransmissions, highest_seq);
}


int main(int argc, char *argv[])
{
    struct sockaddr_in    in_addr, out_addr, from_addr;
    int                   in_sock, out_sock, max_sd, from_ip, cur_ip = INADDR_ANY;
    fd_set                mask, read_mask;
    int                   bytes;
    int                   num;
    struct stream_pkt     strm_pkt;
    struct rt_packet      packet;
    struct ack_pkt        nack;
    struct rt_packet      buffer[WINDOW_SIZE];
    struct timeval        now, diff_time, last_recv_time;
    socklen_t             from_len;
    long int              next_report = REPORT_SEC * 1000000;
    /* Parse commandline args */
    Usage(argc, argv);
    printf("Receiving UDP streams at port %d\n", in_port);
    printf("Sending UDP streams at port %d\n", out_port);

    /* Open socket for receiving udp_stream */
    in_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (in_sock < 0) {
        perror("rt_srv: socket");
        exit(1);
    }

    /* Open socket for receiving udp_stream */
    out_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (out_sock < 0) {
        perror("rt_srv: socket");
        exit(1);
    }

    /* Bind in_sock to listen for incoming messages on app port */
    in_addr.sin_family = AF_INET; 
    in_addr.sin_addr.s_addr = INADDR_ANY; 
    in_addr.sin_port = htons(in_port);

    if (bind(in_sock, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) {
        perror("rt_srv: bind");
        exit(1);
    }

    /* Bind out_sock to listen for incoming messages on client port */
    out_addr.sin_family = AF_INET; 
    out_addr.sin_addr.s_addr = INADDR_ANY; 
    out_addr.sin_port = htons(out_port);

    if (bind(out_sock, (struct sockaddr *)&out_addr, sizeof(out_addr)) < 0) {
        perror("rt_srv: bind");
        exit(1);
    }

    /* Set up mask for file descriptors we want to read from */
    FD_ZERO(&read_mask);
    FD_ZERO(&mask);
    FD_SET(in_sock, &read_mask);
    FD_SET(out_sock, &read_mask); 

    sendto_dbg_init(loss_percent);

    memset(&buffer, 0, WINDOW_SIZE * sizeof(struct rt_packet));


    max_sd = max(in_sock, out_sock);
    //printf("In: %d, Out: %d, Max SD: %d\n",in_sock, out_sock, max_sd);

    gettimeofday(&starts_at, NULL);

    for(;;)
    {
        /* print stats */
        gettimeofday(&now, NULL);
        timersub(&now, &starts_at, &diff_time);
        if(convert_time_to_usec(diff_time) >= next_report){
            next_report += REPORT_SEC * 1000000;
            print_stats(now);
        }
        /* (Re)set mask */
        mask = read_mask;
        /* Wait for message (NULL timeout = wait forever) */
        num = select(max_sd + 1, &mask, NULL, NULL, NULL);
        //printf("Num: %d\n",num);
        if (num > 0) {
            if (FD_ISSET(in_sock, &mask)){
                from_len = sizeof(from_addr);
                bytes = recvfrom(in_sock, &strm_pkt, sizeof(strm_pkt), 0,  
                          (struct sockaddr *)&from_addr, 
                          &from_len);
                from_ip = from_addr.sin_addr.s_addr;

                /*
            
                printf("Received from (%d.%d.%d.%d): Seq %d\n", 
                        (htonl(from_ip) & 0xff000000)>>24,
                        (htonl(from_ip) & 0x00ff0000)>>16,
                        (htonl(from_ip) & 0x0000ff00)>>8,
                        (htonl(from_ip) & 0x000000ff),
                        strm_pkt.seq);
                */
                gettimeofday(&last_recv_time, NULL);
                
                /* place packet in buffer*/
                packet.seq = strm_pkt.seq - 1;
                memcpy(&packet.ts, &last_recv_time, sizeof(last_recv_time));
                memcpy(&packet.data, &strm_pkt.data, sizeof(strm_pkt.data));
                memcpy(&buffer[packet.seq % WINDOW_SIZE], &packet, sizeof(packet));

                total_packets += 1;

                /* save the start time */
                if(total_packets == 1){
                    memcpy(&starts_at, &last_recv_time, sizeof(last_recv_time));
                    printf("Start Time Updated\n");
                }

                /* send rt_packet to client */
                if(cur_ip != INADDR_ANY){
                    sendto_dbg(out_sock, (char *)&packet, sizeof(packet), 0, 
                        (struct sockaddr *)&out_addr, sizeof(out_addr));

                    highest_seq = packet.seq + 1;

                    //printf("sending at Port %d: Seq %d\n", out_port,
                    //        packet.seq);
                }

            } else if(FD_ISSET(out_sock, &mask)){
                // receive nack from out_sock
                from_len = sizeof(from_addr);
                bytes = recvfrom(out_sock, (char *)&nack, sizeof(nack), 0,  
                          (struct sockaddr *)&from_addr, 
                          &from_len);
                from_ip = from_addr.sin_addr.s_addr;
                /*
                printf("Received NACK from (%d.%d.%d.%d): Seq %d\n", 
                        (htonl(from_ip) & 0xff000000)>>24,
                        (htonl(from_ip) & 0x00ff0000)>>16,
                        (htonl(from_ip) & 0x0000ff00)>>8,
                        (htonl(from_ip) & 0x000000ff),
                        nack.ack_no);
                
                */
                /* check if this is a reuest for a new connection */
                if(nack.ack_no < 0){
                    if(cur_ip == INADDR_ANY){
                        /* initlialize new connection */
                        int success = init_new_connection(out_sock, from_ip, from_addr, read_mask, nack.ack_no);
                        if(success){
                            printf("connection established at port %d\n", out_port);
                            cur_ip = from_ip;
                            memcpy(&out_addr, &from_addr, sizeof(from_addr));
                        }else{
                            continue;
                        }
                    }else if(cur_ip != from_ip){
                        /* reject this connection */
                        nack.ack_no = 0;
                        gettimeofday(&now, NULL);
                        memcpy(&nack.ts, &now, sizeof(now));
                        sendto_dbg(out_sock, (char *)&nack, sizeof(nack), 0, 
                            (struct sockaddr *)&from_addr, sizeof(from_addr));
                        printf("Reject request from (%d.%d.%d.%d): Seq %d\n", 
                            (htonl(from_ip) & 0xff000000)>>24,
                            (htonl(from_ip) & 0x00ff0000)>>16,
                            (htonl(from_ip) & 0x0000ff00)>>8,
                            (htonl(from_ip) & 0x000000ff),
                            nack.ack_no);
                    }
                }else{
                    /* this is nack from the client for a lost packet, send the packet */
                    int wnd_idx = nack.ack_no % WINDOW_SIZE;
                    if(nack.ack_no == buffer[wnd_idx].seq){
                        gettimeofday(&now, NULL);
                        /* check if it's too late to resend the packet */
                        if(Cmp_time(nack.ts, now) == 1){
                            /* send rt packet to client */
                            if(! is_empty(buffer[wnd_idx], sizeof(buffer[wnd_idx]))){
                                sendto_dbg(out_sock, (char *)&buffer[wnd_idx], sizeof(buffer[wnd_idx]), 0, 
                                    (struct sockaddr *)&out_addr, sizeof(out_addr));
                                //printf("sending at Port %d: Seq %d\n", out_port,
                                //        buffer[wnd_idx].seq);
                                retransmissions += 1;
                            }
                        }
                    }
                }
            }
        }
        
    }

    return 0;
}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {
    if (argc != 4) {
        Print_help();
    }
    int i = 1;
    loss_percent = atoi(argv[i++]);
    in_port = atoi(argv[i++]);
    out_port = atoi(argv[i++]);
}

static void Print_help() {
    printf("Usage: rt_srv <loss_rate_percent> <app_port> <client_port>\n");
    exit(0);
}

static int Cmp_time(struct timeval t1, struct timeval t2) {
    if      (t1.tv_sec  > t2.tv_sec) return 1;
    else if (t1.tv_sec  < t2.tv_sec) return -1;
    else if (t1.tv_usec > t2.tv_usec) return 1;
    else if (t1.tv_usec < t2.tv_usec) return -1;
    else return 0;
}