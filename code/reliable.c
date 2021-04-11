#include <stdio.h>
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

//test change
struct reliable_state {
    //rel_t means the same as reliable_state (typedef)
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */

    buffer_t* send_buffer;
    buffer_t* rec_buffer;
    
    /* Add your own data fields below this */

    config_common *cc; //common config - use for window, timeout
    int base_seq; //lowest received packet seqno
    void * temp_buf; //buffer of values to be sent
    uint32_t rcv_nxt; //next seqno expected: rec_buffer->next->packet->seqno
    int send_nxt; //next seqno which is unassigned (to be sent)
    int send_wndw; //send window
    int base_send; //lowest sent packet seqno

};
rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
l_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
const struct config_common *cc)
{
    rel_t *r;
    fprintf(stderr,"did this commit work?");

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
    r->temp_buf = xmalloc(500);
    r->send_nxt = 1;
    r->rcv_nxt = 1;
    r->send_wndw = r->cc->window; //not sure abt this sndnxt-base_seq
    r->base_seq = 1;

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
    
    if (nthos(pkt->len>512 || nthos(pkt->len)<0)) {
        fprintf(stderr, "packet length out of bounds");
        return;
    }
    /*if (nthos(pkt->len) != n) {
        fprintf(stderr, "expected packet size differs from actual packet size");
        return;
    }*/
    if (!cksum(pkt->data,n)) {
        fprintf(stderr,"packet checksum doesnt fit");
        //packet has been corrupted --> directly return
        //need to send ack?
        return;
    }
    //check if packet's seqno is lower than next expected rec seqno
   // if (ntohl(pkt->seqno < r->rcv_nxt)) {
        //send ack ?
  //  }
    //ntohs for len, ntohl for seqno
    if (ntohs(pkt->len) == 8) {
        //pkt is an ack packet
        //ack: all packets until but excluding that seqno are acked
        //update lowest seq number if received packet has a higher ackno
        //note that an ack means that all packets with lower ackno have also been received!
        //(no selective acks)
        if (pkt->ackno > r->base_seq) {
            //slide window:
            buffer_remove(r->rec_buffer,pkt->seqno);
        }
        //send other packets
        rel_read(r);
        //rcvwindow doesnt change

    }
    //data packet of len 12
    else if (ntohs(pkt->len) ==12) {
        //end of file, 0 payload
        //receive zero-len payload and have written contents of prev 
        //packets (TODO: check this)--> send EOF 
        conn_output(r->c,r->send_buffer,0);
        rel_destroy(r);

    }
    else {
        //len>8
        //data packet, receiver functionality
        //send ack
        //test maybe need htohs(pkt->len)
        ack_packet ack = {cksum(pkt->data,pkt->len),htohs(pkt->len),htohl(r->base_seq+1)};
        conn_sendpkt(r->c,ack,8);
        free(ack);

        //add to output buffer = rcv buffer (=packets that are printed to stdout)
        buffer_insert(r->rcv_buffer,pkt,0);
        //the received packet is the expected one (lowest seqno in curr windw)
        if (ntohl(pkt->seqno) == (r->base_seq)) {
            //add to outpt buf & write to output
            rel_output(r);
            r->base_seq++;
            //conn_sendpkt(r->c,r->,8);
        }
        
    }
//TODO: buffer out of sequence packets --> how do we know if it's out of sequence/
    //what is expected seqno?
}

/*
reads values from stdin and writes them into the send buffer; then sends them

*/

void
rel_read (rel_t *s)
{
    //sndwnd = sndnxt-snduna; //not a constant
    //update send window
    s->send_wndw = s->send_nxt - s->base_send;

    //check if the next packet is in sending window (= lowest ack + window size)
    while (s->send_nxt < s->base_send + s->send_wndw) {
        //get data from conn_input
        //returns number of bytes received
        int sendPkt = conn_input(s->c, s->temp_buf, 500); 

        if (sendPkt == -1) { //either error or EOF
            rel_destroy(s);
        }      

        else if (sendPkt == 0) {
            //no data is available
            //library will call again once data is available
            return;
        }
        else {
            //we got some data that we can now send

            //loop through all nodes in temp_buf (there are exactly sendPkt #)
            buffer_node* next = s->temp_buf->head;
            packet_t sendme = xmalloc(sendPkt+12);
            for (int i=0;i<sendPkt;i++) {
                sendme->data[i] = ((char *)s->temp_buf)[i]; //cast to char pointer from void *

            }
            sendme->len = htons(12+sendPkt); 
            sendme->cksum = 0x00;
            sendme->cksum = cksum(sendme->data,sendme->len);
            sendme->ackno = htonl(s->rcv_next);
            sendme->seqno = htonl(s->send_nxt);
            int isSent = conn_sendpkt(s->c,sendme,ntohs(sendme->len));
            buffer_insert(s->send_buffer,sendme,getTimeMs());
            free(sendme);

           // packet_t* pkt = xmalloc(sendPkt+12);
            //what i don't understand: we get a packet and then we send it using
            //sendpkt, but then why do we need send_buffer? what does it do?
            //only trieed to send it w sendpkt, need to put into buffer until acked

        }
        s->send_nxt++;
        s->send_wndw = s->send_nxt - s->base_send;
    }
    
    /* Your logic implementation here */
}

/*
writes data from output buffer to std output
go thru receive buffer
*/
void
rel_output (rel_t *r)
{

    int rec_wnd = r->cc->window; //doesnt change
    size_t space = conn_bufspace(r->c);
    //go through nodes in rec_buffer and output in-order packets
    //always look for base_seq, and if that packet found, increase base_seq
    while (curr_node != NULL ) {
        buffer_node_t curr_node = buffer_get_first(r->rec_buffer);
        int out = conn_output(r->c,r->rec_buffer,space);
        if (out==-1) {
            fprintf(stderr,"buffer couldn't output");
        }
        buffer_remove(r->rec_buffer,curr_node->packet->seqno);
        space = conn_bufspace(r->c);

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
        //for each sender, go through all nodes and check timeout
        buffer_node_t curr_node = buffer_get_first(current->send_buffer);
        while (curr_node != NULL) {
            long retr_timer = current->cc->timeout; //in millisecs
            long time_passed = getTimeMs() - curr_node->last_retransmit;
            if (time_passed >= retr_timer) {
                //packet should be resent
                conn_sendpkt(current->c, &curr_node->packet,nthos(curr_node->packet->len));
                //update the time of now to that packet
                buffer_node_t new = {curr_node->packet,getTimeMs(),curr_node->next};
                *curr_node = new;
            }       

        }
        current = rel_list->next;
    }
}

//uses the internal clock to return the current time in milliseconds
long getTimeMs() {
    struct timeval now; 
    gettimeofday(&now , NULL); 
    long nowMs = now.tv_sec ∗ 1000 + now.tv_usec / 1000;
    return nowMs;
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
