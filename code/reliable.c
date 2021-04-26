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

    
    /* Add your own data fields below this */


    //send-specific
    int base_send; //lowest sent packet seqno
    int send_nxt; //next seqno which is unassigned (to be sent)
    buffer_t* send_buffer;

    //receive-specific
    int rcv_nxt; //next seqno expected: next ack = this (largest received seqno+2/ largest rec ack+1)
    buffer_t* rec_buffer;


    void* temp_buf; //buffer of values to be sent (gotten from conn_input)
    int window;
    int timeout;

    //tear down
    int input_eof; //1 if has gotten eof from input (read)
    int rcv_eof; //1 if has received eof from other sender
};
rel_t *rel_list;


// call this function to check whether EOF's have been sent & rec & buffers are empty
// if all of this holds, the connection can be torn down w rel_destroy

int canDestroy(rel_t *r) {
    if ( r->rcv_eof && r->input_eof && !buffer_size(r->send_buffer) && !buffer_size(r->rec_buffer)) {
        fprintf(stderr,"calling rel destroy but sending ack beforehand\n");
        
        return 1;
    } 
    return 0;
    
}

//uses the internal clock to return the current time in milliseconds
long getTimeMs() {
    struct timeval now;
    gettimeofday(&now,NULL);
    return now.tv_sec * 1000 + now.tv_usec / 1000;
}

//creates an ack 
packet_t* create_ack(uint32_t ackno) {
    packet_t* pkt = xmalloc(8);
    pkt->len = htons(8);
    pkt->ackno = htonl(ackno);

    //set cksum to 0 first
    pkt->cksum = 0x0000;
    pkt->cksum = cksum(pkt,8);

    return pkt;
}
// returns 1 if the given buffer is empty (pointer to first node = null)
int isEmpty(buffer_t* buf) {
    if (buffer_get_first(buf) == NULL) {
        return 1;
    } else {
        return 0;
    }
}

//data = char array of 500
//creates a data packet when given pointer to packet which already contains data
packet_t* create_data(packet_t* pkt, uint16_t len_, uint32_t ackno_, uint32_t seqno_) {
    pkt->len = htons(len_);
    pkt->ackno = htonl(ackno_);
    pkt->seqno = htonl(seqno_);

    //set cksum to 0 first 
    pkt->cksum = 0x0000;
    pkt->cksum = cksum(pkt,len_);
    return pkt;
}

//returns 1 if a packet is corrupted and 0 otherwise
int is_corrupted(packet_t* pkt) {
    if ( (ntohl(pkt->ackno) <= 0) || (ntohl(pkt->seqno) <= 0) || (ntohs(pkt->len)>512) ) {
        return 1;
    }

    uint16_t oldsum = pkt->cksum;

    pkt->cksum = 0x0000;
    uint16_t newsum = ~cksum(pkt,ntohs(pkt->len));
    pkt->cksum = oldsum;

    fprintf(stderr,"olsum=%04x and newsum=%04x\n",(oldsum),(newsum));
    return oldsum + newsum == 0xFFFF ? 0 : 1;
}
/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
const struct config_common *cc)
{
    rel_t *r;
    fprintf(stderr,"rel_create was called\n");

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
    r->window = cc->window;
    r->base_send = 1;
    r->send_nxt = 1;
    r->rcv_nxt = 1;
    r->input_eof = 0;
    r->rcv_eof = 0;

    r->temp_buf = xmalloc(500);
    r->timeout = cc->timeout;

    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;
    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;

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
    free(r->temp_buf);


}

// n is the expected length of pkt
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    //fprintf(stderr,"rel_recvpkt was called with packet ackno  %08x and checksum %04x\n", ntohl(pkt->ackno), (pkt->cksum));
    if (is_corrupted(pkt) || ntohs(pkt->len) != n) {
        fprintf(stderr, "packet was corrupted\n");
        //still send an ack 
        packet_t* ack = create_ack(r->rcv_nxt);
        conn_sendpkt(r->c, ack, 8);
        free(ack);
        return;
    }
    uint32_t n_ackno = ntohl(pkt->ackno);
    if (n_ackno > r->base_send) {
        //slide window:
        fprintf(stderr,"ack's number is larger than base\n");
        r->base_send = n_ackno;
        buffer_remove(r->send_buffer,r->base_send);
        rel_read(r);
    }
    if (ntohl(pkt->seqno) < r->rcv_nxt) {
        //received a not yet acked packet, need to send ack
        packet_t* ack = create_ack(r->rcv_nxt);
        conn_sendpkt(r->c,ack,8);
        free(ack);
        return;
    }
    if (ntohs(pkt->len) == 12) {
        fprintf(stderr,"received EOF bc pkt len 12\n");
        r->rcv_eof = 1;
    }
    if ( canDestroy(r)) {
        packet_t* ack = create_ack(r->rcv_nxt);
        conn_sendpkt(r->c,ack,8);
        free(ack);
        rel_destroy(r);
        return;
    }
    if (ntohl(pkt->seqno) < r->rcv_nxt + r->window) {
        if (buffer_contains(r->rec_buffer, ntohl(pkt->seqno))) {

            packet_t* ack = create_ack(r->rcv_nxt);
            conn_sendpkt(r->c, ack, 8);
            free(ack);
            return;
        }

        if (ntohs(pkt->len) >= 12) {

            buffer_insert(r->rec_buffer, pkt, 0);

            if (ntohl(pkt->seqno) == r->rcv_nxt) { //packet is the next expected one-->try to output
                
                rel_output(r);
                packet_t* ack = create_ack(r->rcv_nxt);
                conn_sendpkt(r->c, ack, 8);
                free(ack);
            }
        }
    }
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
            if (s->input_eof) {
                return;
            }
            //send eof
            packet_t* eof = xmalloc(12);
            eof = create_data(eof, 12, s->rcv_nxt,s->send_nxt);
            conn_sendpkt(s->c, eof, ntohs(eof->len));

            s->input_eof = 1;
            buffer_insert(s->send_buffer,eof,getTimeMs());
            free(eof);
        }      

        else if (sendPkt == 0) {
            //no data is available
            //library will call again once data is available
            fprintf(stderr, "nothing was sent because 0 bytes retrieved\n");
            return; //check these returns 
        }
        else {
            //we got some data that we can now send

            //loop through all nodes in temp_buf (there are exactly sendPkt #)
           // struct buffer_node* next = s->temp_buf->head;
            packet_t* sendme = xmalloc(sendPkt+12); //sizeof struct pkt
            
            for (unsigned int i=0;i<sendPkt;i++) {
                sendme->data[i] = ((char *) s->temp_buf)[i]; //cast to char pointer from void *

            }
            sendme = create_data(sendme,12+sendPkt,s->rcv_nxt,s->send_nxt); //12+sendPkt instead of 12
            fprintf(stderr,"sending data from rel_read ... seqno=%08x\n",sendme->seqno);
            
            conn_sendpkt(s->c,sendme,ntohs(sendme->len));
            buffer_insert(s->send_buffer,sendme,getTimeMs());
            free(sendme);

           // packet_t* pkt = xmalloc(sendPkt+12);
            //what i don't understand: we get a packet and then we send it using
            //sendpkt, but then why do we need send_buffer? what does it do?
            //only trieed to send it w sendpkt, need to put into buffer until acked
        }
        s->send_nxt++;
        //s->send_wndw = s->send_nxt - s->base_send;
        if (canDestroy(s)) {
            packet_t* ack = create_ack(s->rcv_nxt);
            conn_sendpkt(s->c,ack,8);
            free(ack);
            rel_destroy(s);
            return;
        } //removed this bc otherwise window test not passed
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
    buffer_node_t* curr_node = r->rec_buffer->head;
    uint32_t currSeq = ntohl(curr_node->packet.seqno);
    uint32_t prevSeq = currSeq -1;
    //loop thru consec. packets, break if cleareed buffer
    while (1) {
        if (prevSeq +1==currSeq) { //theyre following right one after the other
            conn_output(r->c,curr_node->packet.data,ntohs(curr_node->packet.len)-12);
        }
        else { //we have a gap between the packet's seqno's
            r->rcv_nxt=prevSeq+1;
            break;
        }
        //we have next node
        if (curr_node->next != NULL ) {
            prevSeq = currSeq;
            curr_node = curr_node->next;
            currSeq = curr_node->packet.seqno;
        }
        else { //next node =null
            r->rcv_nxt=currSeq+1;
            break;
        }
    }
    //remove all outputtet packets from buffer
    buffer_remove(r->rec_buffer,r->rcv_nxt);
    if (canDestroy(r)) {
        packet_t* ack = create_ack(r->rcv_nxt);
        conn_sendpkt(r->c,ack,8);
        free(ack);
        rel_destroy(r);
        return;
    }
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

            if (getTimeMs() >= current->timeout + curr_node->last_retransmit) {
                //packet should be resent
                conn_sendpkt(current->c, &curr_node->packet,ntohs(curr_node->packet.len));
                //update the time of now to that packet
                buffer_node_t new = {curr_node->packet,getTimeMs(),curr_node->next};
                *curr_node = new;
            } 
            curr_node = curr_node->next; //forgot this one      

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
