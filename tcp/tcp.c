#include <stdio.h>
#include <signal.h>
#include <assert.h>
#include "tcp.h"
#include "unistd.h"

#define FIN_FLAG 0x01
#define SYN_FLAG 0x02
#define RST_FLAG 0x04
#define PSH_FLAG 0x08
#define ACK_FLAG 0x10
#define URG_FLAG 0x20


/* States */
typedef enum{
    S_START, S_CLOSED, S_CONNECTING, S_LISTEN, S_SYN_SENT, S_SYN_ACK_SENT,
    S_SYN_RECEIVED, S_ESTABLISHED, S_FIN_WAIT_1, S_FIN_WAIT_2, S_CLOSE_WAIT,
    S_TIME_WAIT, S_CLOSING, S_LAST_ACK
} state_t;

/* Events causing state transitions*/
typedef enum {
    E_SOCKET_OPEN, E_CONNECT, E_SYN_SENT, E_SYN_ACK_RECEIVED, E_LISTEN,
    E_SYN_RECEIVED, E_SYN_ACK_SENT, E_ACK_RECEIVED, E_ACK_TIME_OUT, E_CLOSE, 
    E_PARTNER_DEAD, E_FIN_RECEIVED
} event_t;

/* Procedure prototypes */
int send_data(const char *buf, int len);
int send_syn(void);
int send_ack(void);
int send_fin(void);
void do_packet(void);
void handle_ack(tcp_u8t flags, tcp_u32t ack_nr);
void handle_data(tcp_u8t flags, tcp_u32t seq_nr, char *data, int data_size);
void handle_syn(tcp_u8t flags, tcp_u32t seq_nr, ipaddr_t their_ip);
void handle_fin(tcp_u8t flags, tcp_u32t seq_nr);
void declare_event(event_t e);
void clear_tcb(void);
int wait_for_ack(void);
int all_acks_received(void);
void ack_these_bytes(int bytes_delivered);
int packet_is_valid(tcp_u32t seq_nr, tcp_u32t ack_nr, tcp_u8t flags,
                    tcp_u16t src_port, tcp_u16t dst_port, int data_sz);

tcp_u16t tcp_checksum(ipaddr_t src, ipaddr_t dst, void *segment, int len);
void tcp_alarm(int sig);
void receive_new_data(int maxlen);
int deliver_received_bytes(char *buf, int maxlen);

int min(int x, int y);
int max(int x, int y);
    

/* TCP control block */
typedef struct tcb {
    ipaddr_t our_ipaddr;
    ipaddr_t their_ipaddr;
    tcp_u16t our_port;
    tcp_u16t their_port;
    tcp_u32t our_seq_nr;
    tcp_u32t their_seq_nr;  /* last byte we acked */
    tcp_u32t ack_nr;        /* the seq nr to ack in next packet */
    tcp_u32t expected_ack;  
    char rcv_data[BUFFER_SIZE];
    int rcvd_data_start;    /* pointer to start of circular buffer */
    int rcvd_data_size;     /* nr of bytes in buffer */
    int rcvd_data_psh;      /* number of bytes to push, (from start of buffer)*/
    char *unacked_data;     /* transmitted data yet to be acked (or resent) */
    int unacked_data_len;   /* length of transmitted data yet to be acked */
    state_t state;          /* stores the current state of the connection */
    tcp_u32t their_previous_seq_nr; /* to detect duplicate packets */
    tcp_u8t their_previous_flags;   /* to detect duplicate packets */
} tcb_t;

/* Pseudo header */
typedef struct pseudo_header {
     ipaddr_t src;
     ipaddr_t dst;
     tcp_u8t zero;
     tcp_u8t ptcl;
     tcp_u16t tcp_length;
} pseudo_hdr_t;

/* TCP header without options */
typedef struct tcp_header {
    tcp_u16t src_port;
    tcp_u16t dst_port;
    tcp_u32t seq_nr;
    tcp_u32t ack_nr;
    tcp_u8t data_offset;
    tcp_u8t flags;
    tcp_u16t win_sz;
    tcp_u16t checksum;
    tcp_u16t urg_pointer;
} tcp_hdr_t;

/* TCP segment header */
typedef struct seg_header {
    pseudo_hdr_t pseudo_header;
    tcp_hdr_t tcp_header;
} seg_header_t;


typedef struct segment segment_t;

static tcb_t tcb = {
    0,       /* out_ipaddr        */
    0,       /* their_ipaddr      */
    0,       /* our_port          */
    0,       /* their_port        */
    0,       /* our_seq_nr        */
    0,       /* their_seq_nr      */
    0,       /* ack_nr            */
    0,       /* expected_ack      */
    "",      /* rcv_data          */
    0,       /* rcvd_data_start   */
    0,       /* rcvd_data_size    */
    0,       /* rcvd_data_psh     */
    "",      /* unacked_data      */
    0,       /* unacked_data_len  */
    S_START, /* state             */
    0,       /* their_previous_seq_nr */
    0,       /* their_previous_flags */
};

static int alarm_went_off = 0; 



/* -----------TCP primitives---------- */
/*      CONNECTION ORIENTED TIER       */
/* ----------------------------------- */


/* Calls ip_init() and clears the tcb.
    This function is idempotent.        
   Returns 0, but -1 on error  */

int tcp_socket(void) {

    if (!my_ipaddr){
        ip_init();
    }
    if (!my_ipaddr) {
        return -1;
    }

    declare_event(E_SOCKET_OPEN);
    tcb.our_ipaddr = my_ipaddr;

    return 0;
}



int tcp_connect(ipaddr_t dst, int port) {

    if (tcb.state != S_CLOSED) {
        return -1;
    }

    declare_event(E_CONNECT);
    tcb.our_port = CLIENT_PORT;
    tcb.their_ipaddr = dst;
    tcb.their_port = port; 

    return send_syn();

}


int tcp_listen(int port, ipaddr_t *src) {
    void (*oldsig)(int);
    
    if (tcb.state != S_CLOSED) {
        return -1;
    }

    tcb.our_port = port;
    /* we don't know their port yet */
    tcb.their_port = 0;
    
    /* reset alarm_went_off */
    alarm_went_off = 0;
    /* use our own alarm fucntion when alarm goes of */
    oldsig = signal(SIGALRM, tcp_alarm);

    declare_event(E_LISTEN);
    while (alarm_went_off == 0 && tcb.state != S_ESTABLISHED) {
        do_packet();
        if (tcb.state == S_SYN_RECEIVED) {
            send_syn();
            if (tcb.state != S_ESTABLISHED) {
                return -1;
            }
        }
    }
    
    if (alarm_went_off) {
        /* reset alarm_went_of and call original alarm function */
        alarm_went_off = 0;
        oldsig(SIGALRM);
        return -1;
    } else {
        /* restore old signal handler */
        signal(SIGALRM, oldsig);
    }
    *src = tcb.their_ipaddr;
    return 0;
}


int tcp_close(void){

    if (tcb.state != S_ESTABLISHED
        && tcb.state != S_CLOSE_WAIT) {
        return -1;
    }

    declare_event(E_CLOSE);
    send_fin();
    return 0;
}



int tcp_read(char *buf, int maxlen) {

    int delivered_bytes;
    
    if (tcb.state != S_ESTABLISHED &&
        tcb.state != S_FIN_WAIT_1 &&
        tcb.state != S_FIN_WAIT_2 &&
        tcb.state != S_CLOSING &&
        tcb.state != S_CLOSE_WAIT &&
        tcb.state != S_LAST_ACK &&
        tcb.state != S_CLOSED) {
        
        /* if not in one of these states, tcp_read is not willing to help */
        return -1;
    }
    
    /*  if the buffer is empty... */
    if ( tcb.rcvd_data_size == 0 ) {
    
        /* and a fin is received, return 0; everything went fine*/
        if (tcb.state == S_CLOSING || 
            tcb.state == S_CLOSE_WAIT || 
            tcb.state == S_LAST_ACK) {
            return 0;
        } 
        
        /*and the connection is closed, return ERROR, no read possible */
        if (tcb.state == S_CLOSED) {
            return -1;
        }    
    }
    

    /* if we are in one of these states we haven't received a fin yet, */
    /* so we try to receive new data */
    if (tcb.state == S_ESTABLISHED
        || tcb.state == S_FIN_WAIT_1
        || tcb.state == S_FIN_WAIT_2) {
        
        /* returns immediately if PSH-flagged data in the buffer */
        receive_new_data(maxlen);
    }
    
    /* copy bytes to user buffer */
    delivered_bytes = deliver_received_bytes(buf, maxlen);
    
    return delivered_bytes;

}



/* This method helps tcp_read to receive new data by calling do_packet() */
void receive_new_data(int maxlen) {

    int bytes_to_read;
    void (*oldsig)(int);
    
    bytes_to_read = min(maxlen, BUFFER_SIZE);
    /* reset alarm_went_off */
    alarm_went_off = 0;
    /* use our own alarm fucntion when alarm goes of */
    oldsig = signal(SIGALRM, tcp_alarm);

    /*
      While there's no data we have to push AND there's room to read more:
      handle incomming packets
    */

    /* call do_packet while conditions are met */
    while ( alarm_went_off == 0 && 
            tcb.rcvd_data_psh == 0 && 
            tcb.rcvd_data_size < bytes_to_read &&
            /* make sure we didn't receive a fin: */
            tcb.state != S_CLOSED &&
            tcb.state != S_CLOSE_WAIT &&
            tcb.state != S_LAST_ACK) {
            
        do_packet();
    }
     
    if (alarm_went_off) {
        /* reset alarm_went_off and call original alarm function */
        alarm_went_off = 0;
        oldsig(SIGALRM);
    }  else {
        /* restore old signal handler */
        signal(SIGALRM, oldsig);
    }
}



/* This method helps tcp_read to copy the received data from the circular buffer
    to the user buffer */

int deliver_received_bytes(char *buf, int maxlen) {
    
    int bytes_to_copy, size, first_chunk_sz;
    
    bytes_to_copy = min(maxlen, tcb.rcvd_data_size);
    
    /* copy first chunk out of circular buffer*/
    first_chunk_sz = BUFFER_SIZE - tcb.rcvd_data_start;
    size = min(bytes_to_copy, first_chunk_sz);
    memcpy(buf, &tcb.rcv_data[tcb.rcvd_data_start], size);

    /* possibly copy second chunk if delivered data wraps in buffer */
    if (bytes_to_copy > first_chunk_sz) {
        memcpy(&buf[size], tcb.rcv_data, bytes_to_copy - first_chunk_sz);
    }

    /* adjust buffer pointers */
    tcb.rcvd_data_size -= bytes_to_copy;
    tcb.rcvd_data_psh = max(tcb.rcvd_data_psh - bytes_to_copy, 0);
    tcb.rcvd_data_start = (tcb.rcvd_data_start + bytes_to_copy) % BUFFER_SIZE;

    return bytes_to_copy;
}



int tcp_write(const char *buf, int len){
    
    int bytes_sent = 0, bytes_left, data_sz;
    char *buf_pointer;
    
    bytes_left = len;
    buf_pointer = (char *) buf;

    if (tcb.state != S_ESTABLISHED) {
        return -1;
    }

    while (bytes_left) {
        data_sz = min(MAX_TCP_DATA, bytes_left);

        bytes_sent = send_data(buf_pointer, data_sz);
        
        if (bytes_sent == -1) {break;}
        
        buf_pointer += bytes_sent;
        bytes_left -= bytes_sent;
        
    }
    
    if (bytes_left == len) {
        /* will also happen if len=0*/
        return -1;
    } else {
        return len - bytes_left;
    }

}


/* ----------------------------------- */
/*              STATE TIER             */
/* ----------------------------------- */


void do_packet(void) {
    ipaddr_t their_ip;
    tcp_u16t src_port, dst_port, win_sz;
    tcp_u32t seq_nr, ack_nr;
    tcp_u8t flags;
    char data[MAX_TCP_DATA];
    int data_sz = 0, rcvd;
 
    rcvd = recv_tcp_packet(&their_ip, &src_port, &dst_port, 
                        &seq_nr, &ack_nr, &flags, &win_sz, data, &data_sz);


    if (rcvd != -1) {

        /* only accept syn if packet is legal and state is LISTEN */    
        if (tcb.state == S_LISTEN && 
            (flags & SYN_FLAG) && 
            !(flags & ACK_FLAG)) {
            
            tcb.their_port = src_port;
        }
        
        if (!packet_is_valid(seq_nr, ack_nr, flags, 
                            src_port, dst_port, data_sz)) {

            return;
        }

        /* only handle packet if it belongs to current socket */
        if (dst_port == tcb.our_port && src_port == tcb.their_port){
        
            handle_ack(flags, ack_nr);
            handle_data(flags, seq_nr, data, data_sz);
            handle_syn(flags, seq_nr, their_ip);
            handle_fin(flags, seq_nr);
            
            /* we store this to detect duplicate packets later on */
            tcb.their_previous_seq_nr = seq_nr;
            tcb.their_previous_flags = flags;
        }
    }

}


void handle_ack(tcp_u8t flags, tcp_u32t ack_nr) {

    if (!(ACK_FLAG & flags)){
        return;
    }

    if (ack_nr == tcb.expected_ack) {

        tcb.our_seq_nr = ack_nr;
        tcb.unacked_data_len = 0;

        if (tcb.state == S_ESTABLISHED) {return;}

        if (tcb.state == S_SYN_ACK_SENT ||
            tcb.state == S_FIN_WAIT_1 ||
            tcb.state == S_LAST_ACK ||
            tcb.state == S_CLOSING) {

            declare_event(E_ACK_RECEIVED);
        }
    }
}



void handle_data(tcp_u8t flags, tcp_u32t seq_nr, char *data, int data_size) {

    tcp_u32t fresh_data_start, fresh_data_size;
    int size, first_chunk_size, free_buffer_space, second_chunk_size, 
        end_of_buffer, free_at_end_of_buffer, start_of_second_chunk;

    /* number of bytes we can accept */
    free_buffer_space = BUFFER_SIZE - tcb.rcvd_data_size;
    
    if (data_size > 0 && free_buffer_space > 0) {

        /* okay, we are able to store more data */

        /* calculate start of data that's new to us */
        /* do_packet guarantees that seq_nr is never ahead 
           of tcb.their_seq_nr, i.e. that */
        fresh_data_start = tcb.their_seq_nr - seq_nr;
        fresh_data_size = data_size - fresh_data_start;

        if (fresh_data_size > 0  && fresh_data_start <= MAX_TCP_DATA) {

            /* it is what we expect;
               - at least 1 byte we don't have yet
               - and directly following the bytes we do have 
                 (fresh_data_start is unsigned and would wrap 
                  around if negative, and become greater than MAX_TCP_DATA)*/

            /* how much are we going to store? */
            size = min(free_buffer_space, fresh_data_size);
            
            
            /* before we proceed, send an ack */
            /* increase the number of bytes we acked */
            tcb.ack_nr += size;
            if ( send_ack() == -1 ) {
                /* on error decrease ack_nr and discard data in packet */
                tcb.ack_nr -= size;
                return;
            }


            /* check whether data in buffer has already been wrapped */
            end_of_buffer = tcb.rcvd_data_start + tcb.rcvd_data_size;
            if ( end_of_buffer >= BUFFER_SIZE ) {

                /* buffer is wrapped already, copy to buffer in one chunck */
                end_of_buffer -= BUFFER_SIZE;
                memcpy(&tcb.rcv_data[end_of_buffer], 
                                &data[fresh_data_start], size);

            } else {

                /* copy data to buffer and wrap at end of buffer if needed */
                free_at_end_of_buffer = BUFFER_SIZE - end_of_buffer;
                first_chunk_size = min(size, free_at_end_of_buffer);

                memcpy(&tcb.rcv_data[end_of_buffer], 
                            &data[fresh_data_start], first_chunk_size);

                if (first_chunk_size < size) {

                    start_of_second_chunk = fresh_data_start + first_chunk_size;
                    second_chunk_size = size - first_chunk_size;
                    
                    /* copy second chunck */
                    memcpy(tcb.rcv_data, 
                            &data[start_of_second_chunk], second_chunk_size);
                }

            }

            tcb.rcvd_data_size += size;
            tcb.their_seq_nr += size;

            if (PSH_FLAG & flags) {
                tcb.rcvd_data_psh = tcb.rcvd_data_size;
            }


        } else {
        
            /* no fresh data, but we need to ack in case the last ack was lost*/
            if (tcb.their_previous_seq_nr == seq_nr) {
            /* we got a duplicate on our hands; send ack again*/
                send_ack();
            }
        }
    }
    
    /* data should always fit in buffer */
    assert(tcb.rcvd_data_size <= BUFFER_SIZE);
}


void handle_syn(tcp_u8t flags, tcp_u32t seq_nr, ipaddr_t their_ip) {


    if (!(SYN_FLAG & flags)){
        return;
    }


    if (tcb.state == S_LISTEN) {
        
        if (!(ACK_FLAG & flags)) {
            tcb.their_ipaddr = their_ip;
            tcb.their_seq_nr = seq_nr + 1;
            tcb.ack_nr = seq_nr + 1;
            declare_event(E_SYN_RECEIVED);
        }

    } else if (tcb.state == S_SYN_SENT) {

        if (all_acks_received()) {

            declare_event(E_SYN_ACK_RECEIVED);
            tcb.their_seq_nr = seq_nr + 1;
            tcb.ack_nr = seq_nr + 1;
            send_ack();
        }
        
    } else if (tcb.state == S_ESTABLISHED) {
        
        if ( (tcb.their_previous_seq_nr == seq_nr) &&
             (tcb.their_previous_flags & SYN_FLAG) ) {
            /* we got a duplicate on our hands; send ack again */
            send_ack();
        }
        
    }
}


void handle_fin(tcp_u8t flags, tcp_u32t seq_nr) {
    int s;
    if (FIN_FLAG & flags) {
        
        s = tcb.state;
        
        if (s == S_ESTABLISHED || s == S_FIN_WAIT_1 || s == S_FIN_WAIT_2) {
        
            tcb.their_seq_nr = seq_nr + 1;
            tcb.ack_nr = seq_nr + 1;
            send_ack();
            declare_event(E_FIN_RECEIVED);
            
        } else if (s == S_CLOSE_WAIT || s == S_LAST_ACK) {
        
            /* we already received a fin; let's see if it's a duplicate */
            if ( (tcb.their_previous_seq_nr == seq_nr) &&
                 (tcb.their_previous_flags & FIN_FLAG) ) {
                 /* yes, it's a duplicate; ack it again */
                 send_ack();
            }
        }
    }   
}



/*
  buf       Buffer with bytes to send
  len       Number of bytes to send

  Returns number of bytes sent, or -1 on error.
*/

int send_data(const char *buf, int len) {
    
    int bytes_sent = 0;
    char flags = PSH_FLAG | ACK_FLAG;
    int retransmission_allowed = MAX_RETRANSMISSION;

    while (retransmission_allowed--) {
        bytes_sent = send_tcp_packet(tcb.their_ipaddr, tcb.our_port, 
            tcb.their_port, tcb.our_seq_nr, tcb.ack_nr, flags, 1, buf, len);

        if(bytes_sent == -1){
            return -1;

        } else {
            tcb.expected_ack = tcb.our_seq_nr + bytes_sent;
            tcb.unacked_data_len = bytes_sent;
        }
                           
        if (wait_for_ack()){
            return bytes_sent;
        } 
    }
    return -1;
}


/*
    Sends a syn packet, and waits for ack.
    Returns 0, but -1 on error.
*/

int send_syn(void) {
    
    char *buf;
    char flags = PSH_FLAG | SYN_FLAG;
    int retransmission_allowed = MAX_RETRANSMISSION;
    int result;

    if (tcb.state != S_CONNECTING) {
        flags |= ACK_FLAG;
    }

    while (retransmission_allowed--) {
    
        /* send syn packet */
        result = send_tcp_packet(tcb.their_ipaddr, tcb.our_port, 
            tcb.their_port, tcb.our_seq_nr, tcb.ack_nr, flags, 1, buf, 0);
        
        /* check result */
        if(result == -1){
            return -1;
        } else {
        
            tcb.expected_ack = tcb.our_seq_nr + 1;
            if (flags & ACK_FLAG) {
                declare_event(E_SYN_ACK_SENT);
            } else {
                declare_event(E_SYN_SENT);
            }
        }
        
        /* wait for ack */          
        if (wait_for_ack() && tcb.state == S_ESTABLISHED){
            return 0;
        } else {
            declare_event(E_ACK_TIME_OUT);
        }
    }
    declare_event(E_PARTNER_DEAD);
    return -1;
}



/*
    Sends a fin packet, and waits for ack.
    Returns 0, but -1 on error.
*/

int send_fin(void) {
    
    char *buf;
    char flags = PSH_FLAG | FIN_FLAG | ACK_FLAG;
    int retransmission_allowed = MAX_RETRANSMISSION;
    int result;

    while (retransmission_allowed--) {
    
        /* send fin packet */
        result = send_tcp_packet(tcb.their_ipaddr, tcb.our_port, 
            tcb.their_port, tcb.our_seq_nr, tcb.ack_nr, flags, 1, buf, 0);
        
        /* check result */
        if(result == -1){
            return -1;
        } else {
            tcb.expected_ack = tcb.our_seq_nr + 1;
        }
        
        /* wait for ack */          
        if (wait_for_ack() && tcb.state != S_FIN_WAIT_1){
            return 0;
        }
    }
    declare_event(E_PARTNER_DEAD);
    return -1;
}


/*
    Sends an ack packet.
    Returns 0, but -1 on error.
*/

int send_ack(void) {

    char *buf;
    char flags = PSH_FLAG | ACK_FLAG;

    flags |= PSH_FLAG;
    flags |= ACK_FLAG;

    return send_tcp_packet(tcb.their_ipaddr, tcb.our_port, 
            tcb.their_port, tcb.our_seq_nr, tcb.ack_nr, flags, 1, buf, 0);
}



int wait_for_ack(void){

    void (*oldsig)(int);
    unsigned oldtimo;

    alarm_went_off = 0;
    oldsig = signal(SIGALRM, tcp_alarm);
    oldtimo = alarm(RTT);
    
    while (alarm_went_off == 0 && !all_acks_received()) {
        do_packet();
    }

    signal(SIGALRM, oldsig);    
    alarm(oldtimo);
    alarm_went_off = 0;
    
    return all_acks_received();
}

/* Check validity of ports, flags, seq nr and ack nr.
   returns 0 on error, 1 on succes */ 

int packet_is_valid(tcp_u32t seq_nr, tcp_u32t ack_nr, tcp_u8t flags,
                    tcp_u16t src_port, tcp_u16t dst_port, int data_sz) {

    tcp_u32t diff;
    
    /* only accept packet if it belongs to current socket */
    if (dst_port != tcb.our_port || src_port != tcb.their_port) {
        return 0;
    }
    
    if (tcb.state == S_LISTEN) {
    
        if ( !(flags & SYN_FLAG) || (flags & ACK_FLAG) ) {
            /* in this state accept syn, but no ack */
            return 0;
        }
    }
    
    if (tcb.state == S_SYN_SENT) {
        /* in this state only accept syn+ack packets */
        if ( !(flags & ACK_FLAG) || !(flags & SYN_FLAG)) {
            return 0;
        }
        /* is this a reasonable ack number? */
        diff = tcb.expected_ack - ack_nr;
        if ( diff > MAX_TCP_DATA ) {
            return 0;
        }
    }
    
    /* check seq and ack only if this is not a syn packet */
    if ( !(flags & SYN_FLAG) ) {
        /*    todo: check sequence number */
        
        if ( !(flags & ACK_FLAG) ) {
            return 0;
        }
        
        /* is this a reasonable ack number? */
        diff = tcb.expected_ack - ack_nr;
        if ( diff > MAX_TCP_DATA ) {
            return 0;
        }    
    } 
    
    
    /* we don't handle data in a syn/fin packet */
    if ( (flags & SYN_FLAG) || (flags & FIN_FLAG) ) {
        if (data_sz > 0) {
            return 0;
        }
    }
    
    if (data_sz > MAX_TCP_DATA){
        return 0;
    }
    
    return 1;
}


void clear_tcb(void) {

    /* fast forward dirty seq_nr if the last packet wasn't acked. */
    tcb.our_seq_nr += tcb.unacked_data_len;
    /* clear some variables */
    tcb.their_seq_nr = 0;
    tcb.their_ipaddr = 0;
    tcb.their_port = 0;
    tcb.rcvd_data_start = 0;
    tcb.rcvd_data_size = 0;
    tcb.unacked_data_len = 0;
}



void tcp_alarm(int sig){
    alarm_went_off = 1;
}



/* performs state transition based on event and current state */
void declare_event(event_t e) {
    int error = 0;
    int s;
    
    s = tcb.state;

    if (s == S_START && e == E_SOCKET_OPEN) {
        tcb.state = S_CLOSED;

    } else if (e == E_SOCKET_OPEN) {
        /* reset connection */
        tcb.state = S_CLOSED;
        clear_tcb();
        
    } else if (s == S_CLOSED && e == E_CONNECT) {
        tcb.state = S_CONNECTING;
        
    } else if (s == S_CLOSED && e == E_LISTEN) {
        tcb.state = S_LISTEN;

    } else if (s == S_CONNECTING && e == E_SYN_SENT) {
        tcb.state = S_SYN_SENT;
        
    } else if (s == S_SYN_SENT && e == E_SYN_ACK_RECEIVED) {
        tcb.state = S_ESTABLISHED;

    } else if (s == S_SYN_SENT && e == E_ACK_TIME_OUT) {
        tcb.state = S_CONNECTING;
        
    } else if (s == S_LISTEN && e == E_SYN_RECEIVED) {
        tcb.state = S_SYN_RECEIVED;
        
    } else if (s == S_SYN_RECEIVED && e == E_SYN_ACK_SENT) {
        tcb.state = S_SYN_ACK_SENT;

    } else if (s == S_SYN_ACK_SENT && e == E_ACK_RECEIVED) {
        tcb.state = S_ESTABLISHED;

    } else if (s == S_SYN_ACK_SENT && e == E_ACK_TIME_OUT) {
        tcb.state = S_SYN_RECEIVED;

    } else if (s == S_ESTABLISHED && e == E_CLOSE) {
        tcb.state = S_FIN_WAIT_1;

    } else if (s == S_FIN_WAIT_1 && e == E_FIN_RECEIVED) {
        tcb.state = S_CLOSING;
        
    } else if (s == S_FIN_WAIT_1 && e == E_ACK_RECEIVED) {
        tcb.state = S_FIN_WAIT_2;
  
    } else if (s == S_FIN_WAIT_2 && e == E_FIN_RECEIVED) {
        tcb.state = S_CLOSED;
        clear_tcb();
        
    } else if (s == S_ESTABLISHED && e == E_FIN_RECEIVED) {
        tcb.state = S_CLOSE_WAIT;    
        
    } else if (s == S_CLOSING && e == E_ACK_RECEIVED) {
        tcb.state = S_CLOSED;
        clear_tcb();
    
    } else if (s == S_CLOSE_WAIT && e == E_CLOSE) {
        tcb.state = S_LAST_ACK;
     
    } else if (s == S_LAST_ACK && e == E_ACK_RECEIVED) {
        tcb.state = S_CLOSED;
        clear_tcb();
        
    } else if (e == E_PARTNER_DEAD) {
        tcb.state = S_CLOSED;
        clear_tcb();
        
    } else {
        error = 1;
    }
    
#ifdef DEBUG
    if (error) {
        printf("\n %s: UNSUPPORTED TRANSITION!\n", inet_ntoa(my_ipaddr));
        printf("current state: %d, event: %d\n",s,e);
    }
#endif
}



int all_acks_received(void) { 
    return tcb.our_seq_nr == tcb.expected_ack;
}

/* ----------------------------------- */
/*         CONNECTIONLESS TIER         */
/* ----------------------------------- */

/* returns -1 on error */
int send_tcp_packet(ipaddr_t dst, 
        tcp_u16t src_port,
        tcp_u16t dst_port, 
        tcp_u32t seq_nr, 
        tcp_u32t ack_nr, 
        tcp_u8t flags, 
        tcp_u16t win_sz, 
        const char *data, 
        int data_sz) {
    
    int bytes_sent;
    tcp_u32t tcp_sz, hdr_sz;
    tcp_hdr_t *tcp;
    char segment[MAX_TCP_SEGMENT_LEN];

    hdr_sz = sizeof(tcp_hdr_t);
    tcp_sz = hdr_sz + data_sz;
    tcp = (tcp_hdr_t *) segment;

    tcp->src_port = htons(src_port);
    tcp->dst_port = htons(dst_port);
    tcp->seq_nr = htonl(seq_nr);
    tcp->ack_nr = htonl(ack_nr);
    tcp->data_offset = (hdr_sz >> 2) << 4;
    tcp->flags = flags;
    tcp->win_sz = htons(win_sz);
    tcp->checksum = 0x00;
    tcp->urg_pointer = 0;

    memcpy(&segment[hdr_sz], data, data_sz);
    
    tcp->checksum = tcp_checksum(my_ipaddr, dst, tcp, tcp_sz);

    bytes_sent = ip_send(dst,IP_PROTO_TCP, 2, tcp, tcp_sz );

    if (bytes_sent == -1) {
        return -1;
    } else {
        return bytes_sent - hdr_sz;
    }
}



int recv_tcp_packet(ipaddr_t *src_ip, 
        tcp_u16t *src_port,
        tcp_u16t *dst_port, 
        tcp_u32t *seq_nr, 
        tcp_u32t *ack_nr, 
        tcp_u8t *flags,
        tcp_u16t *win_sz, 
        char *data, 
        int *data_sz) {
    
    int len = 0;
    tcp_hdr_t *tcp;
    ipaddr_t dst_ip;
    tcp_u16t proto = 0, id, chksm = 1;
    char *segment;
    tcp_u8t hdr_sz;
    


    proto = 0;
    len = ip_receive(src_ip, &dst_ip, &proto, &id, &segment);
        
    if ( len == -1 || proto != IP_PROTO_TCP ){
        return -1;
    }
    
    tcp = (tcp_hdr_t *) segment;
    
    chksm = tcp_checksum(*src_ip, tcb.our_ipaddr, tcp, len);
    if (chksm) {
        return -1;
    }
    
    *src_port = ntohs(tcp->src_port);
    *dst_port = ntohs(tcp->dst_port);
    *seq_nr   = ntohl(tcp->seq_nr);
    *ack_nr   = ntohl(tcp->ack_nr);
    *flags    = tcp->flags;
    *win_sz   = ntohs(tcp->win_sz);
        
    hdr_sz = (tcp->data_offset) >> 2;
    *data_sz = len - (int)hdr_sz;
    
    memcpy(data, &segment[(int)hdr_sz], *data_sz);
    free(segment);
    return *data_sz;

}


 /*
 * 16-bit one complement check sum over segment and pseudo header
 */
 
tcp_u16t tcp_checksum(ipaddr_t src, ipaddr_t dst, void *segment, int len) {
    
    unsigned short *sp;
    unsigned long sum, oneword = 0x00010000;
    int count; 

    /*assemble pseudoheader*/
    pseudo_hdr_t pseudo_hdr;
    pseudo_hdr.src = (src);
    pseudo_hdr.dst = (dst);
    pseudo_hdr.zero = 0;
    pseudo_hdr.ptcl = IP_PROTO_TCP;
    pseudo_hdr.tcp_length = htons(len);
    
                
    /* calculate sum of one complements over pseudo header*/
    count = sizeof(pseudo_hdr_t) >> 1;
    
    for (sum = 0, sp = (unsigned short *)&pseudo_hdr; count--; ) {
        sum += *sp++;
        if (sum >= oneword) { /* wrap carry into low bit */
            sum -= oneword;
            sum++;
        }
    }


    /* add sum of one complements over segment */
    count = len >> 1;    
    for (sp = (unsigned short *)segment; count--; ) {
        sum += *sp++;
        if (sum >= oneword) { /* wrap carry into low bit */
            sum -= oneword;
            sum++;
        }
    }        
    
    /* possibly add the last byte */
    if (len & 1) {
#ifdef LITTLE_ENDIAN
        sum += ( (unsigned short) *((char *) sp) ) &  0x00ff; 
#else
        sum += ((unsigned short) *((char *) sp)) << 8;
#endif
        if (sum >= oneword) { /* wrap carry into low bit */
            sum -= oneword;
            sum++;
        }
    }
    return ~sum;
}


int min(int x, int y) {
    return ((x) < (y) ? (x) : (y));
}

int max(int x, int y) {
    return ((x) > (y) ? (x) : (y));
}

