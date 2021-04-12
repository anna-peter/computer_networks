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

    struct config_common *cc; //common config - use for window, timeout
    void * temp_buf; //buffer of values to be sent
    int rcv_nxt; //next seqno expected: next ack = this
    int send_nxt; //next seqno which is unassigned (to be sent)
    int send_wndw; //send window
    int base_send; //lowest sent packet seqno
    int window;
};
rel_t *rel_list;
//uses the internal clock to return the current time in milliseconds
long getTimeMs() {
    struct timeval now;
    gettimeofday(&now,NULL);
    return now.tv_sec * 1000 + now.tv_usec / 1000;
}

//creates an ack 
packet_t* create_ack(uint32_t ackno) {
    packet_t* pkt = xmalloc(8);
    pkt->cksum = 0x0000;
    pkt->cksum = cksum(pkt,8);
    pkt->len = htons(8);
    pkt->ackno = htonl(ackno);
    return pkt;
}

//data = char array of 500
//creates a data packet when given pointer to packet which already contains data
packet_t* create_data(packet_t* pkt, uint16_t len, uint32_t ackno, uint32_t seqno) {
    pkt->cksum = 0x0000;
    pkt->cksum = cksum(pkt,len);
    pkt->len = htons(len);
    pkt->ackno = htonl(ackno);
    pkt->seqno = htonl(seqno);
    return pkt;
}
//returns 1 if a packet is corrupted and 0 otherwise
int is_corrupted(packet_t* pkt) {
    if ( ntohs(pkt->len)>512 || ntohl(pkt->seqno)<=0 || ntohl(pkt->ackno)<=0 ) {
        fprintf(stderr, "packet length was out of bounds: length = %04x\n", ntohs(pkt->len));
        return 1;
    }
    uint16_t oldsum = pkt->cksum;
    pkt->cksum = 0x0000;
    uint16_t newsum = ~cksum(pkt,ntohs(pkt->len));
    pkt->cksum = oldsum;
    return (oldsum+newsum==0xFFFF) ? 0 : 1;
}
/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
const struct config_common *cc)
{
    rel_t *r;
    fprintf(stderr,"rel_create was called");

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
    r->send_wndw = cc->window; //not sure abt this sndnxt-base_seq
    r->base_send = 1;
    r->window = cc->window;

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
    free(r->temp_buf);


}

// n is the expected length of pkt
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    fprintf(stderr,"rel_recvpkt was called with packet ackno  %08x and checksum %04x\n", ntohl(pkt->ackno), pkt->cksum);
    if (is_corrupted(pkt)) {
        fprintf(stderr, "packet was corrupted\n");
        packet_t* ack = create_ack(r->rcv_nxt);
        conn_sendpkt(r->c,ack,ntohs(8));
        free(ack);
        return;
    }

    if (ntohs(pkt->len) != n) {
        fprintf(stderr, "expected packet size differs from actual packet size\n");
        return;
    }
    if (ntohl(pkt->ackno) > r->base_send) {
        //slide window:
        fprintf(stderr,"ack's number is larger than base\n");
        r->base_send = pkt->ackno;
        buffer_remove(r->send_buffer,r->base_send);
        rel_read(r);
    }
    if (ntohl(pkt->seqno) < r->rcv_nxt) {
        packet_t* ack = create_ack(r->rcv_nxt);
        conn_sendpkt(r->c,ack,8);
        free(ack);
        return;
    }
    if (ntohs(pkt->len) == 8) {
        fprintf(stderr,"received ack\n");
        //pkt is an ack packet
        //ack: all packets until but excluding that seqno are acked
        //update lowest seq number if received packet has a higher ackno
        //note that an ack means that all packets with lower ackno have also been received!
        //(no selective acks)
        //send other packets
        rel_read(r);
        //rcvwindow doesnt change

    }
    //data packet of len 12
    else if (ntohs(pkt->len) ==12 && !(r->rec_buffer)) {
        //end of file, 0 payload and rec buffer is empty
        //receive zero-len payload and have written contents of prev 
        //packets (TODO: check this)--> send EOF 
        fprintf(stderr,"received EOF bc pkt len 12, calling rel_destroy\n");
        conn_output(r->c,pkt,0);
        rel_destroy(r);
    }
    else {
        //len>8
        //data packet, receiver functionality
        //send ack
        fprintf(stderr,"received data packet\n");
        
        //add to output buffer = rcv buffer (=packets that are printed to stdout)
        buffer_insert(r->rec_buffer,pkt,getTimeMs());
        //the received packet is the expected one (lowest seqno in curr windw)
        if (ntohl(pkt->seqno) == (r->rcv_nxt)) {
            //add to outpt buf & write to output
            packet_t* ack = create_ack(r->rcv_nxt);
            conn_sendpkt(r->c,ack,8);
            free(ack);
            rel_output(r);
            r->rcv_nxt++;
        }
        
    }
//TODO: buffer out of sequence packets 
}

/*
reads values from stdin and writes them into the send buffer; then sends them

*/

void
rel_read (rel_t *s)
{
    fprintf(stderr,"rel_read was called with base_send=%08x, window size=%i, send_nxt=%08x\n",s->base_send,s->window,s->send_nxt);
    //sndwnd = sndnxt-snduna; //not a constant
    //update send window
   // s->send_wndw = s->send_nxt - s->base_send;

    //check if the next packet is in sending window (= lowest ack + window size)
    while (s->send_nxt < s->base_send + s->window) {
        fprintf(stderr,"entered while loop\n");
        //get data from conn_input
        //returns number of bytes received
        int sendPkt = conn_input(s->c, s->temp_buf, 500); 
        fprintf(stderr,"sending so many bytes: %i\n",sendPkt);
        if (sendPkt == -1) { //either error or EOF
            //send eof
            packet_t* eof = xmalloc(12);
            eof = create_data(eof, 12, s->rcv_nxt,s->send_nxt);
            conn_output(s->c,eof,0);
            buffer_insert(s->send_buffer,eof,getTimeMs());
            free(eof);
        }      

        else if (sendPkt == 0) {
            //no data is available
            //library will call again once data is available
            return;
        }
        else {
            //we got some data that we can now send

            //loop through all nodes in temp_buf (there are exactly sendPkt #)
           // struct buffer_node* next = s->temp_buf->head;
            packet_t* sendme = xmalloc(sendPkt+12);
            for (int i=0;i<sendPkt;i++) {
                sendme->data[i] = ((char *)s->temp_buf)[i]; //cast to char pointer from void *

            }
            sendme = create_data(sendme,htons(12+sendPkt),htonl(s->rcv_nxt),htonl(s->send_nxt));
            int isSent = conn_sendpkt(s->c,sendme,ntohs(sendme->len));
            buffer_insert(s->send_buffer,sendme,getTimeMs());
            free(sendme);

           // packet_t* pkt = xmalloc(sendPkt+12);
            //what i don't understand: we get a packet and then we send it using
            //sendpkt, but then why do we need send_buffer? what does it do?
            //only trieed to send it w sendpkt, need to put into buffer until acked

        }
        s->send_nxt++;
        //s->send_wndw = s->send_nxt - s->base_send;
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
    size_t space = conn_bufspace(r->c);
    fprintf(stderr,"rel_output was called with bufferspace = %zu\n", space);

    //go through nodes in rec_buffer and output in-order packets
    //always look for rcv_nxt, and if that packet found, increase rcv_nxt
    buffer_node_t* curr_node = buffer_get_first(r->rec_buffer);
    while (curr_node != NULL ) {
        int out = conn_output(r->c,r->rec_buffer,space);
        if (out==-1) {
            fprintf(stderr,"buffer couldn't output");
            return;
        }
        fprintf(stderr,"removing packet with seqno=%08x\n",curr_node->packet.seqno);
        buffer_remove(r->rec_buffer,curr_node->packet.seqno);
        curr_node = buffer_get_first(r->rec_buffer);
        space = conn_bufspace(r->c);
    }
    if (!r->rec_buffer && !r->send_buffer) {
        //rec & send buffers are empty
        fprintf(stderr,"calling rel_destroy because emptied buffers\n");
        rel_destroy(r);
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
        buffer_node_t* curr_node = buffer_get_first(current->send_buffer);
        while (curr_node != NULL) {
            long retr_timer = current->cc->timeout; //in millisecs
            long now = getTimeMs();
            long time_passed = now - curr_node->last_retransmit;
            if (time_passed >= retr_timer) {
                //packet should be resent
                conn_sendpkt(current->c, &curr_node->packet,ntohs(curr_node->packet.len));
                //update the time of now to that packet
                buffer_node_t new = {curr_node->packet,getTimeMs(),curr_node->next};
                *curr_node = new;
            }       

        }
        current = rel_list->next;
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
