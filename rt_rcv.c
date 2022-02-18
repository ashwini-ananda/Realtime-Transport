#include "sendto_dbg.h"
#include "udp_stream_common.h"
#include <sys/stat.h>
#include <math.h>
#include<time.h>

static void Usage(int argc, char *argv[]);
static void Print_help();
static int Cmp_time(struct timeval t1, struct timeval t2);
static int in_port, out_port, latency_window, loss_percent;
static char* server_ip;
struct timeval start_in, start_out;

#define min(a,b)            (((a) < (b)) ? (a) : (b))
#define max(a,b)            (((a) < (b)) ? (b) : (a))


static void Usage(int argc, char *argv[]);
static void Print_help();

static int WINDOW_SIZE = 1786;
/* fix it later */
static struct timeval rtt = {0,40000}, start_time = {0,0};
static int total_packets = 0, packets_sent = 0, packets_received = 0, highest_recv_seq = 0, packets_delivered = 0;
double min_delay = 5000.0, max_delay = 0.0, total_delay = 0.0, avg_delay = 0.0;

void create_ack_pkt(struct ack_pkt *ack, int ack_no){
    struct timeval now;
    ack->ack_no = ack_no;
    gettimeofday(&now, NULL);
    memcpy(&ack->ts, &now, sizeof(now));
}

void create_nack_pkt(struct ack_pkt *ack, int ack_no, struct timeval latest_delivery){
    struct timeval diff_time;
    ack->ack_no = ack_no;
    memcpy(&ack->ts, &latest_delivery, sizeof(latest_delivery));
}

void create_strm_pkt(struct stream_pkt *strm_pkt, struct rt_packet *packet){
    strm_pkt->seq = packet->seq;
    strm_pkt->ts_sec = packet->ts.tv_sec;
    strm_pkt->ts_usec = packet->ts.tv_usec;
    memcpy(&strm_pkt->data, &packet->data, sizeof(packet->data));
}

double convert_timeval_to_ms(struct timeval t_time){
    return t_time.tv_sec * 1000 + t_time.tv_usec / 1000.0;
}

void update_min_avg_max_delay(double base_delta){
    base_delta = abs(base_delta);
    total_packets++;
    min_delay = min(base_delta, min_delay);
    max_delay = max(max_delay, base_delta);
    total_delay = total_delay + base_delta;
    avg_delay = total_delay / total_packets;
}

double convert_time_to_usec(struct timeval t_time){
    long int t = t_time.tv_sec * 1000000;
    return t * 1.0 + t_time.tv_usec;
}

void print_stats(struct timeval cur_time){
    struct timeval diff_time;
    timersub(&cur_time, &start_time, &diff_time);
    double in_usec = convert_time_to_usec(diff_time);
    double in_sec = in_usec / 1000000.0;
    printf("packets received: %d, avg rate: %lf packets/sec, data_sent: %lf MB, avg rate: %lf Mbps\n",
                                        packets_received, packets_received / in_sec, MAX_DATA_LEN * packets_received / 1000000.0,
                                        MAX_DATA_LEN * packets_received * 8.0 / in_usec);
    printf("packets sent: %d, avg rate: %lf packets/sec, data_sent: %lf MB, avg rate: %lf Mbps\n",
                                        packets_sent, packets_sent / in_sec, MAX_DATA_LEN * packets_sent / 1000000.0,
                                        MAX_DATA_LEN * packets_sent * 8.0 / in_usec);
    printf("Delays -> Min: %lf ms, Max: %lf ms, Avg: %lf ms\n", min_delay, 
                                                             max_delay, 
                                                             avg_delay);
    printf("Highest Received Seq: %d,Packets Lost: %d, Loss Rate: %f\n", highest_recv_seq,
                                (highest_recv_seq - packets_received), (highest_recv_seq - packets_received) * 1.0 / highest_recv_seq);
}




int init_new_connection(int sock, struct sockaddr_in addr, int ip, fd_set read_mask){
    struct ack_pkt ack_out, ack_in;
    struct timeval now, diff_time;
    fd_set mask;
    time_t t;
    socklen_t from_len;

    int buf_len = 10;
    struct ack_pkt ack_buf[buf_len];
    int count = 0;

    srand((unsigned) time(&t));
    int ack_no = -1000 - rand();

    for(;;){
        //ack_out.ack_no = ack_no;
        //gettimeofday(&ack_out.ts, NULL);
        create_ack_pkt(&ack_out, ack_no);
        /* send ack packet to the server*/
        sendto_dbg(sock, (char *)&ack_out, sizeof(ack_out), 0, 
            (struct sockaddr *)&addr, sizeof(addr));
        //printf("sending ack packet for seq %d\n", ack_out.ack_no);
        /* place ack_pkt in buffer */
        memcpy(&ack_buf[count % buf_len], &ack_out, sizeof(ack_out));
        count++;
        /* wait for server to reply back*/
        mask = read_mask;
        int num = select(FD_SETSIZE, &mask, NULL, NULL, &rtt);
        if (num > 0) {
            if (FD_ISSET(sock, &mask)){
                from_len = sizeof(addr);
                int bytes = recvfrom(sock, &ack_in, sizeof(ack_in), 0,  
                          (struct sockaddr *)&addr, 
                          &from_len);
                int from_ip = addr.sin_addr.s_addr;
                gettimeofday(&now, NULL);
                if (ip == from_ip){
                    /* check for which ack we got this reply back */
                    for(int i = 0; i < min(count, buf_len); i++){
                        if(ack_buf[i].ack_no == ack_in.ack_no){
                            /* calculate rtt */
                            timersub(&now, &ack_buf[i].ts, &diff_time);
                            memcpy(&rtt, &diff_time, sizeof(diff_time));
                            /* fix it later */
                            //rtt.tv_sec = 2;
                            /* update min, max, avd delay */
                            timersub(&now, &ack_in.ts, &diff_time);
                            update_min_avg_max_delay(convert_timeval_to_ms(diff_time));
                            printf("Connection established with (%d.%d.%d.%d):\n", 
                            (htonl(from_ip) & 0xff000000)>>24,
                            (htonl(from_ip) & 0x00ff0000)>>16,
                            (htonl(from_ip) & 0x0000ff00)>>8,
                            (htonl(from_ip) & 0x000000ff));
                            printf("RTT: %lf ms\n", convert_timeval_to_ms(rtt));
                            return 1;
                        }else if(ack_in.ack_no == 0){
                            printf("Connection rejected from (%d.%d.%d.%d):\n", 
                            (htonl(from_ip) & 0xff000000)>>24,
                            (htonl(from_ip) & 0x00ff0000)>>16,
                            (htonl(from_ip) & 0x0000ff00)>>8,
                            (htonl(from_ip) & 0x000000ff));
                            return 0;
                        }
                    }
                }    
            }
        }else{
            /*timeout*/
            ack_no -= 1;
            continue;
        }
    }
}

void update_timeout(struct timeval *timeout, struct timeval *t_time){
    memcpy(timeout, t_time, sizeof(struct timeval));
    return;
}


int is_empty(struct rt_packet buf, size_t size)
{
    struct rt_packet zero;
    memset(&zero, 0, sizeof(struct rt_packet));
    return !memcmp(&zero, &buf, sizeof(struct rt_packet));
}


int main(int argc, char *argv[])
{
    struct sockaddr_in    out_addr, in_addr, from_addr;
    socklen_t             from_len;
    struct hostent        h_ent;
    struct hostent        *p_h_ent;
    int                   host_num;
    int                   in_sock, out_sock, from_ip;
    fd_set                mask, read_mask;
    int                   bytes, num;
    int                   wnd_idx;
    struct rt_packet      packet;
    struct rt_packet      recv_wnd[WINDOW_SIZE];
    struct timeval        delivery_times[WINDOW_SIZE];
    struct stream_pkt     strm_pkt;
    struct ack_pkt        ack, nack;
    struct timeval        now, timeout, diff_time, min_rtt, last_recv_time;
    int                   next_expected_seq = 0, next_expected_delivery = 0;
    int                   timeout_event_delivery = 0;
    long int              next_report = REPORT_SEC * 1000000;
    double                base_delta;

    /* Parse commandline args */
    Usage(argc, argv);
    printf("Receiving UDP streams at port %d\n", in_port);
    printf("Sending UDP streams at port %d\n", out_port);

    /* Open socket for receiving udp_stream */
    in_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (in_sock < 0) {
        perror("rt_rcv: socket");
        exit(1);
    }

    /* Open socket for sending udp_stream */
    out_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (out_sock < 0) {
        perror("rt_srv: socket");
        exit(1);
    }

    /* Convert string IP address (or hostname) to format we need */
    p_h_ent = gethostbyname(server_ip);
    if (p_h_ent == NULL) {
        printf("rt_rcv: gethostbyname error.\n");
        exit(1);
    }
    memcpy(&h_ent, p_h_ent, sizeof(h_ent));
    memcpy(&host_num, h_ent.h_addr_list[0], sizeof(host_num));

    /* Bind in_sock to listen for incoming messages on in port */
    in_addr.sin_family = AF_INET; 
    in_addr.sin_addr.s_addr = host_num; 
    in_addr.sin_port = htons(in_port);

    /* Bind out_sock to send messages on out port */
    out_addr.sin_family = AF_INET; 
    out_addr.sin_addr.s_addr = INADDR_ANY; 
    out_addr.sin_port = htons(out_port);

    int flag = 1;  
    if (-1 == setsockopt(out_sock, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag))) {  
        perror("setsockopt fail");  
    }  

    if (bind(out_sock, (struct sockaddr *)&out_addr, sizeof(out_addr)) < 0) {
        perror("rt_rcv: bind");
        exit(1);
    }

    /* Set up mask for file descriptors we want to read from */
    FD_ZERO(&read_mask);
    FD_ZERO(&mask);
    FD_CLR(in_sock, &read_mask);
    FD_SET(in_sock, &read_mask);

    /* setup a latency timestamp for calculating delivery time */
    struct timeval latency = {0, latency_window * 1000};

    /* initialize sendto_dbg with loss_rate */
    sendto_dbg_init(loss_percent);

    int success = init_new_connection(in_sock, in_addr, host_num, read_mask);
    if(!success){
        exit(0);
    }
    /* send the confirmation of handshake packet to server */
    create_ack_pkt(&ack, 0);
    sendto_dbg(in_sock, (char *)&ack, sizeof(ack), 0, 
              (struct sockaddr *)&in_addr, sizeof(in_addr));

    update_timeout(&timeout, &rtt);

    /* initialize buffer and delivery times */

    memset(&recv_wnd, 0, WINDOW_SIZE * sizeof(struct rt_packet));
    memset(&delivery_times, 0, WINDOW_SIZE * sizeof(struct timeval));
    gettimeofday(&start_time, NULL);

    for(;;)
    {
        /* print stats */
        gettimeofday(&now, NULL);
        timersub(&now, &start_time, &diff_time);
        if(convert_time_to_usec(diff_time) >= next_report){
            next_report += REPORT_SEC * 1000000;
            print_stats(now);
        }
        /* (Re)set mask */
        mask = read_mask;
        /* Wait for message (NULL timeout = wait forever) */
        
        num = select(FD_SETSIZE, &mask, NULL, NULL, &timeout);
        if (num > 0) {
            /* receive incoming messages from server */
            from_len = sizeof(from_addr);
            //memset(&packet, 0, sizeof(packet));
            bytes = recvfrom(in_sock, &packet, sizeof(packet), 0,  
                          (struct sockaddr *)&from_addr, 
                    &from_len);
            from_ip = from_addr.sin_addr.s_addr;
            /*
            printf("Received from (%d.%d.%d.%d): Seq %d\n", 
                        (htonl(from_ip) & 0xff000000)>>24,
                        (htonl(from_ip) & 0x00ff0000)>>16,
                        (htonl(from_ip) & 0x0000ff00)>>8,
                        (htonl(from_ip) & 0x000000ff),
                        packet.seq);
            */
            /* update timestamp to denote the delivery time */
            gettimeofday(&last_recv_time, NULL);



            
            /* check if it is a deliverable data packet */
            if(packet.seq >= next_expected_delivery){
                timersub(&last_recv_time, &packet.ts, &diff_time);
                base_delta = convert_timeval_to_ms(last_recv_time) - convert_timeval_to_ms(packet.ts);
                    
                /* update min, max, avg delay */
                update_min_avg_max_delay(base_delta);
                /* update packet timestamp to be the delivery time */
                timeradd(&last_recv_time, &latency, &packet.ts);
                
                /* check if it is a new/duplicate/response to lost packet */
                wnd_idx = packet.seq % WINDOW_SIZE;
                if(packet.seq >= next_expected_seq){
                    /* this is a new packet */ 
                    if(packet.seq > next_expected_seq){
                        /* packet(s) lost, check whether it's possible to recover lost packet(s) */
                        //if(min_delay * 2 <= latency_window){
                            for(int i = next_expected_seq; i < packet.seq; i++){
                                create_nack_pkt(&nack, i, packet.ts);
                                sendto_dbg(in_sock, (char *)&nack, sizeof(nack), 0, 
                                            (struct sockaddr *)&in_addr, sizeof(in_addr));
                                /*update delivery time for the nack packets to be the latest delivery time*/
                                memcpy(&delivery_times[i % WINDOW_SIZE], &packet.ts, sizeof(packet.ts));
                            }
                        //}
                    }
                    /* place packet into buffer */
                    memcpy(&recv_wnd[wnd_idx], &packet, sizeof(packet));
                    /* increment next expected seuence */
                    next_expected_seq = packet.seq + 1;
                    /*update delivery time for the received packet */
                    memcpy(&delivery_times[wnd_idx], &packet.ts, sizeof(packet.ts));
                    packets_received++;
                    highest_recv_seq = packet.seq + 1;
                }else if(packet.seq){
                    /* this is response to a nack, update the delivery timestamp from the delivery_times buffer to maintain synchronization */
                    if(is_empty(recv_wnd[wnd_idx], sizeof(recv_wnd[wnd_idx]))){
                        packets_received++;
                    }
                    memcpy(&packet.ts, &delivery_times[wnd_idx], sizeof(delivery_times[wnd_idx]));
                    /* place packet into buffer */
                    memcpy(&recv_wnd[wnd_idx], &packet, sizeof(packet));
                }
                /*save the time of first recv in start time */
                if(packets_received == 1){
                    memcpy(&start_time, &last_recv_time, sizeof(last_recv_time));
                }
            }
        }else{
            /*timeout occured */
            if(timeout_event_delivery){
                /*deliver packets */
                //printf("Next Extected delivery: %d, sequence %d\n",next_expected_delivery, next_expected_seq);
                wnd_idx = next_expected_delivery % WINDOW_SIZE;
                if(!is_empty(recv_wnd[wnd_idx], sizeof(recv_wnd[wnd_idx])) && recv_wnd[wnd_idx].seq == next_expected_delivery){
                    create_strm_pkt(&strm_pkt, &recv_wnd[wnd_idx]);
                    strm_pkt.seq += 1;
                    sendto(out_sock, (char *)&strm_pkt, sizeof(strm_pkt), 0,
                                (struct sockaddr*)&out_addr, sizeof(out_addr));
                    //printf("Sending at port %d seq %d\n", out_port, strm_pkt.seq);
                        /* reinit the space in recv_wnd */
                    memset(&recv_wnd[wnd_idx], 0, sizeof(recv_wnd[wnd_idx]));
                    packets_sent++;
                }
                    next_expected_delivery += 1;
            }else{
                /* check if we didnt receive the anything yet, which means that the last handshake ack_pkt is lost */
                if(next_expected_seq == 0){
                    create_ack_pkt(&ack, 0);
                    sendto_dbg(in_sock, (char *)&ack, sizeof(ack), 0, 
                            (struct sockaddr *)&in_addr, sizeof(in_addr));
                }else{
                    /*send nacks again for lost packets */
                    for(int i = next_expected_delivery; i <= next_expected_seq; i++){
                        wnd_idx = i % WINDOW_SIZE;
                        if(is_empty(recv_wnd[wnd_idx], sizeof(recv_wnd[wnd_idx]))){
                            create_nack_pkt(&nack, i, delivery_times[wnd_idx]);
                            sendto_dbg(in_sock, (char *)&nack, sizeof(nack), 0, 
                                        (struct sockaddr *)&in_addr, sizeof(in_addr));
                        }
                    }
                }
                
            }
        }
        /* update time out */
        gettimeofday(&now, NULL);
        wnd_idx = next_expected_delivery % WINDOW_SIZE;
        
        if(!is_empty(recv_wnd[wnd_idx], sizeof(recv_wnd[wnd_idx]))){
            timersub(&recv_wnd[wnd_idx].ts, &now, &diff_time);
            if(Cmp_time(rtt, diff_time) == 1){
                timeout_event_delivery = 1;
                update_timeout(&timeout, &diff_time);
                //printf("Timeout: %lf\n", convert_timeval_to_ms(timeout));
                continue;
            }
        }
        timeout_event_delivery = 0;
        update_timeout(&timeout, &rtt);
        //printf("Timeout: %lf\n", convert_timeval_to_ms(timeout));
    }

    return 0;
}

/* Read commandline arguments */
static void Usage(int argc, char *argv[])
{
    if (argc != 5)
    {
        Print_help();
    }

    int i = 1;
    loss_percent = atoi(argv[i++]);
    server_ip = strtok(argv[i++], ":");
    if (server_ip == NULL) {
        printf("Error: no server IP provided\n");
        Print_help();
    }   
    printf("IP: %s\n",server_ip);
    in_port = atoi(strtok(NULL, ":"));
    out_port = atoi(argv[i++]);
    latency_window = atoi(argv[i++]);
}

static void Print_help()
{
    printf("Usage: rt_rcv <loss_rate_percent> <server_ip>:<server_port> <app_port> <latency_window>\n");
    exit(0);
}



/* Returns 1 if t1 > t2, -1 if t1 < t2, 0 if equal */
static int Cmp_time(struct timeval t1, struct timeval t2) {
    if      (t1.tv_sec  > t2.tv_sec) return 1;
    else if (t1.tv_sec  < t2.tv_sec) return -1;
    else if (t1.tv_usec > t2.tv_usec) return 1;
    else if (t1.tv_usec < t2.tv_usec) return -1;
    else return 0;
}
