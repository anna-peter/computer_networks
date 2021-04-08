include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"
#include "buffer.h"

struct reliable_state {
    //rel_t means the same as reliable_state (typedef)
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */

    /* Add your own data fields below this */
    // ...
    buffer_t* send_buffer;
    // ...
    buffer_t* rec_buffer;
    // ...
    config_common *cc;
    int base_seq; //lowest received packet seqno
    int window; //window size



};
rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
l_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
const struct config_common *cc)
{
    rel_t *r;

    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));

    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }

    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
    rel_list->prev = &r->next;
    rel_list = r;

    /* Do any other initialization you need here... */
    r->cc = cc;
    // ...
    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;
    // ...
    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;
    // ...

    return r;
}

void
rel_destroy (rel_t *r)
{
    if (r->next) {
        r->next->prev = r->prev;
    }
    *r->prev = r->next;
    conn_destroy (r->c);

    /* Free any other allocated memory here */
    buffer_clear(r->send_buffer);
    free(r->send_buffer);
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
    // ...
    free(r->cc);

}

// n is the expected length of pkt
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    
    //rcv.nxt= next seqno expected

    if (nthos(pkt->len) != n) {
        fprintf(stderr, "expected packet size differs from actual packet size");
        abort ();
    }
    if (ntohs(pkt->len) == 8) {
        //pkt is an ack packet
        //ack: all packets until but excluding that seqno
        //check if it is the expected one:
        //update lowest seq number if received packet
        if (pkt->ackno>r->base_seq) {
            //slide window:
            buffer_remove(r->rec_buffer,pkt->seqno)
        }
        //send other packets
        rel_read(r);
        //set snduna = max(snduna,ackno)
        //rcvwindow doesnt change

    }
    //data packet of len 12
    else if (ntohs(pkt->len) ==12) {
        //end of file, 0 payload
        //receive zero-len payload and have written contents of prev 
        //packets (TODO: check this)--> send EOF 
        conn_output(r->c,r->send_buffer,0);
        //should destroy?
        rel_destroy(r);

    }
    else {
        //len>8
        //data packet, receiver functionality
        
        //this code is to get current time - for last retransmit
        struct timespec spec;
        clock_gettime(CLOCK_MONOTONIC, &spec); 
        long nowMs;
        nowMs = round(spec.tv_nsec / 1.0e6);

        //add to output buffer
        buffer_insert(r->send_buffer,pkt,nowMs);
        rel_output(r)
        
    }
    //rel_destroy(r);
//TODO: buffer out of sequence packets --> how do we know if it's out of sequence/
    //what is expected seqno?
    /* Your logic implementation here */
}

/*
reads values from stdin and writes them into the send buffer; then sends them

*/
void
rel_read (rel_t *s)
{
    sendPkt = conn_input(s);
    isSent = conn_sendpkt(sendPkt, s->send_buffer->head->packet);
    if (isSent == -1) {
        rel_destroy(s)
    }
    else if (isSent == 0) {
        //no data is available
        //library will call again once data is available
        return;
    }
    
    /* Your logic implementation here */
}

void
rel_output (rel_t *r)
{
    //sndwnd = sndnxt-snduna; //not a constant
    space = conn_bufspace(r->c);
    out = conn_output(r->c, r->send_buffer,r->send_buffer->head->packet->len)
    if (out == -1) {
        fprintf(stderr,"buffer couldn't output")
    } 
    /* Your logic implementation here */
}

void
rel_timer ()
{
    // Go over all reliable senders, and have them send out
    // all packets whose timer has expired
    rel_t *current = rel_list;
    
    while (current != NULL) {
        // ...
        retr_timer = current->cc->timeout; //in millisecs
        struct timespec spec;
        clock_gettime(CLOCK_MONOTONIC, &spec); 
        long nowMs;
        nowMs = round(spec.tv_nsec / 1.0e6); //gives us time in ms
        time_passed = now - current->send_buffer->head->last_retransmit;
        if (time_passed > retr_timer) {
            //packet should be resent
            rel_output(current);
        }       
        current = current->next;
    }
}
/*
to run code (test):
cd reliable/code
make
./reliable
./reliable 2000 localhost:2001
open second terminal: 
./reliable 2001 localhost:2000

*/
