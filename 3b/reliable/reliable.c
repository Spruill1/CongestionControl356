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
#include <stdbool.h>
#include <inttypes.h>

#include "rlib.h"

//Reliable TCP States will be later transferred to rlib.h

#define RST_CLOSED			0
#define RST_LISTEN			1
#define RST_SYN_SENT		2
#define RST_SYN_RCVD		3
#define RST_ESTABLISHED		4
#define RST_FIN_WAIT_1		5
#define RST_FIN_WAIT_2		6
#define RST_CLOSING			7
#define RST_CLOSE_WAIT		8
#define RST_LAST_ACK		9
#define RST_READ			10
#define RST_WRITE			11


// example uses different server and client states, thoughts? -John

#define ACK_HEADER_SIZE		12
#define PKT_HEADER_SIZE		16
#define MAX_DATA_SIZE		1000

/*
 This struct will keep track of packets in our sending/receiving windows
 */
typedef struct window_entry{
	struct window_entry *next;			/* Linked list for traversing all windows */
	struct window_entry *prev;

	packet_t pkt;
	struct timespec sen;

	bool valid;
	int timeout;

}window_entry;

struct reliable_state {

	conn_t *c;			/* This is the connection object */

	/* Add your own data fields below this */
	struct timespec start_time;
	struct config_common *cc;
	struct sockaddr_storage *ss;

	window_entry *sending_window;
	window_entry *receiving_window;

	//Sender
	uint32_t lastSeqAcked;
	uint32_t lastSeqWritten;
	uint32_t lastSeqSent;
	uint32_t next_seqno;
	bool sent_EOF;  //have we sent an EOF packet?
	bool sender_finished;

	//Receiver
	uint32_t nextSeqExpected;
	uint32_t lastSeqRead;
	uint32_t lastSeqReceived;
	bool got_EOF;  //have we received an EOF packet?
	bool receiver_finished;

	int pid;
};
rel_t *rel_list;

void printPacket(packet_t *pkt, rel_t *r){
	if(ntohs(pkt->len)==ACK_HEADER_SIZE){
		fprintf(stderr, "Ack ackno=%d | pid=%d\n", ntohl(pkt->ackno), r->pid);
		return;
	}
	fprintf(stderr, "Packet #=%d | l=%d | pid=%d\n",ntohl(pkt->seqno), ntohs(pkt->len), r->pid);

}

//Method Declarations
int windowList_smartAdd(rel_t *r, packet_t *pkt);
window_entry* windowList_dequeue(rel_t *r, window_entry **head);

/*
 * Processes Acks server side, frees up the window depending on the ack.
 * Ignores Acks not in window. Updates lastSeqAcked.
 */
void process_ack(rel_t *r, packet_t* pkt){
	// TODO: redo this method
	//check if packet is in window
	uint32_t ackno = pkt->ackno;
	if(ackno < r->lastSeqAcked || r->lastSeqSent<ackno){
		fprintf(stderr, "INFO: received ack for %d seqno, not in window %d - %d\n",pkt->ackno,r->lastSeqAcked,r->lastSeqAcked+r->cc->window);
	}

	//seqno in flush packets
	window_entry *current = r->sending_window;
	while(current!=NULL && current->pkt.seqno<=ackno && ackno<=r->lastSeqSent){
		current = windowList_dequeue(r, &r->sending_window);
		if(current!=NULL)
			free(current);
		else
			printf("we done fucked up again\n");
		current = r->sending_window;
	}

	if(r->sending_window==NULL && r->sent_EOF){
		//sent an EOF packet and everything has been ACKed.
		r->sender_finished = true;
		if(r->receiver_finished) rel_destroy(r);
		return;
	}

	r->lastSeqAcked = ackno-1;

	//call rel_read
	rel_read(r);
}

void
send_ack(rel_t *r){
	// TODO: send an ack for the next seq expected

	/*
	 Coming into this method the next sequence number expected should have been updated to
	 the first sequence number which we were unable to shift the window past.
	 */

	//make the ack
	packet_t ackPkt;
	ackPkt.len = htons(ACK_HEADER_SIZE);
	ackPkt.ackno = htonl(r->nextSeqExpected);
	memset(&(ackPkt.cksum),0,sizeof(uint16_t));
	ackPkt.cksum = cksum((void*)(&ackPkt),ACK_HEADER_SIZE);

	//send the ack
	conn_sendpkt(r->c, &ackPkt, ACK_HEADER_SIZE);
}

/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
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
	rel_list = r;

	r->next_seqno = 1;

	//Initialize the window
	r->sending_window = NULL;
	r->receiving_window = NULL;

	//Sender
	r->lastSeqAcked = 0;
	r->lastSeqWritten = 0;
	r->lastSeqSent = 0;

	//Receiver
	r->nextSeqExpected = 1;
	r->lastSeqRead = 0;
	r->lastSeqReceived = 0;

	r->pid = getpid();

	/* Do any other initialization you need here */
	//Initialize timer
	clock_gettime(CLOCK_MONOTONIC,&r->start_time);

	//Allocate ss and cc, exactly one should be NULL
	if(ss){
		r->ss = xmalloc(sizeof(struct sockaddr_storage));
		memcpy(r->ss,ss,sizeof(struct sockaddr_storage));
		cc = NULL;
	} else if(cc){
		r->cc = xmalloc(sizeof(struct config_common));
		memcpy(r->cc,cc,sizeof(struct config_common));
		ss = NULL;
	} else{
		return NULL;
	}

	return r;
}

void
rel_destroy (rel_t *r)
{

	//Manage linked list
	if (r->next)
		r->next->prev = r->prev;
	*r->prev = r->next;

	struct timespec end_time;
	clock_gettime(CLOCK_MONOTONIC,&end_time);
	fprintf(stderr, "File transfer was of %ld milliseconds\n",(end_time.tv_nsec - r->start_time.tv_nsec)/(long)(1000000));

	conn_destroy (r->c);

	/* Free any other allocated memory here */
	// free windows
	window_entry *temp_entry = r->sending_window;
	while(temp_entry->next != NULL){
		//free(temp_entry->pkt);
		temp_entry = temp_entry->next;
		free(temp_entry->prev);
	}
	free(temp_entry);

	temp_entry = r->receiving_window;
	while(temp_entry->next != NULL){
		//free(temp_entry->pkt);
		temp_entry = temp_entry->next;
		free(temp_entry->prev);
	}
	free(temp_entry);

	//Don't worry about the connection, rlib frees the connection pointer.
	if(r->ss)
		free(r->ss);
	if(r->cc)
		free(r->cc);
	free(r);
}


void
rel_demux (const struct config_common *cc,
		const struct sockaddr_storage *ss,
		packet_t *pkt, size_t len)
{
	//leave it blank here!!!
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
	printPacket(pkt, r);
	// Check packet formation
	if ((size_t) ntohs(pkt->len) < n){
		return;
	}
	// Check checksum
	uint16_t received_checksum = pkt->cksum;
	memset(&(pkt->cksum),0,sizeof(pkt->cksum));

	if( cksum((void*)pkt, n) != received_checksum){
		return;
	}
	// enforce host byte order
	pkt->len = ntohs (pkt->len);
	pkt->ackno = ntohl (pkt->ackno);

	// make sure packet length is valid.
	if (pkt->len > ACK_HEADER_SIZE){
		pkt->seqno = ntohl(pkt->seqno);
	} else if(pkt->len < ACK_HEADER_SIZE || pkt->len > (MAX_DATA_SIZE+PKT_HEADER_SIZE)){
		fprintf(stderr, "Got packet of invalid size.\n");
		return;
	}

	// Do some stuff with the packets
	// Start by looking for Acks
	if (pkt->len == ACK_HEADER_SIZE){
		process_ack(r,pkt);
	}

	// must be data if it's not corrupted and not an ACK
	else {
		int result = windowList_smartAdd(r,pkt);
		rel_output(r);
		send_ack(r);
	}
}

void windowList_enqueue(rel_t *r, window_entry *w, window_entry **head){
	window_entry *current;
	if(*head == NULL){
		//window_list is null
		*head = w;
		w->next = NULL;
		w->prev = NULL;
		return;
	}
	current = *head;
	//find end
	while(current->next){
		current = current->next;
	}
	current->next = w;
	w->prev = current;
	w->next = NULL;
}

/*
 * Adds a packet to the receiving end window.
 * Fills in the blanks in the window.
 * returns -1 when the packet cannot be added
 *          0 when the packet is redundant
 *          1 in success
 */
int windowList_smartAdd(rel_t *r, packet_t *pkt){

	uint32_t seqno = pkt->seqno;
	struct window_entry *w = r->receiving_window;

	if(r->got_EOF){
		return 0;
	} else if(seqno<r->nextSeqExpected || seqno>r->nextSeqExpected+r->cc->window){
		//ignore packet that has already been processed or that is too far ahead
		fprintf(stderr,"INFO: Package of seqno %d was not added. Already processed or far ahead.\n", seqno);
		return -1;
	} else if(r->receiving_window == NULL){
		w = (window_entry *)xmalloc(sizeof(window_entry));
		memset(&w->pkt, 0, sizeof(packet_t));
		w->pkt.seqno = r->nextSeqExpected;
		w->valid = false;
		r->receiving_window = w;
		w->next = NULL;
		w->prev = NULL;
	}

	int safetyvar_memleak = 0;
	//Find and close gaps!
	window_entry *current = r->receiving_window;
	while(current->pkt.seqno <= seqno){
		int current_seqno = current->pkt.seqno;
		if(safetyvar_memleak>r->cc->window+10){
			fprintf(stderr, "ERROR: MEMLEAK at SMARTADD!\n");
		}
		//make next if necessary
		if(current->next == NULL || current->next->pkt.seqno != (current_seqno+1)){
			//Next window does not exist. Create subsequent window and insert
			w = (window_entry *)xmalloc(sizeof(window_entry));
			memset(&w->pkt.data, 0, sizeof(packet_t));
			w->pkt.seqno = current_seqno + 1;
			w->valid = false;
			w->next = current->next;
			w->prev = current;
			current->next = w;
			continue;
		}

		if(current_seqno == seqno){
			if(!current->valid){
				//Next window exists, check if valid, update if necessary
				memcpy(&current->pkt, pkt, sizeof(packet_t));
				current->valid = true;
				return 1;
			} else{
				if(memcmp(pkt, &current->pkt, pkt->len)==0)
					return 0; //packet was already there!
				else{
					fprintf(stderr, "ERROR: SAME PACKET SEQNO, DIFFERENT DATA");
					return -1;
				}
			}
		}

		current = current->next;
		safetyvar_memleak++;
	}
	fprintf(stderr, "ERROR: Could not smart add. Not smart enough...\n");
	return -1;
}

window_entry* windowList_dequeue(rel_t *r, window_entry **head){
	window_entry *w;
	if(!*head){
		w = NULL;
		return NULL;
	}
	w = *head;
	*head = w->next;
	return w;

}


void
rel_read (rel_t *r)
{
	if(r->c->sender_receiver == RECEIVER)
	{
		//if already sent EOF to the sender
		//  return;
		//else
		//  send EOF to the sender
		if (r->receiver_finished){
			return;
		}
		else {
			packet_t eof;
			eof.len =  htons(PACKET_HEADER_SIZE);
			eof.ackno = htonl(0);
			eof.rwnd = htonl(r->cc->window);
			eof.seqno = htonl(r->seqno); r->seqno++;
			memset(&(eof.cksum),0,sizeof(uint16_t));
			eof.cksum = cksum((void*)(&eof),PACKET_HEADER_SIZE);
			conn_sendpkt(r->c, &eof, PACKET_HEADER_SIZE);

			r->receiver_finished = true;
			return;
		}

	}
	else //run in the sender mode
	{
		int bytes_read = 0;
		int window_size = r->lastSeqWritten - r->lastSeqAcked;
		packet_t packet;
		memset(&(packet.data),0,MAX_DATA_SIZE);
		uint32_t packet_size = 0;

		while(1){
			//Check if we can create a new window entry
			if(window_size > r->cc->window || window_size<0){
				fprintf(stderr,"ERROR: Window size greater than maximum permitted window size or negative");
				return;
			} else if (window_size == r->cc->window){
				//Window is full!
				return;
			}else if((bytes_read = conn_input(r->c, packet.data, MAX_DATA_SIZE)) == 0){
				//Nothing to read
				return;
			}/* else if(bytes_read<0 && errno==EIO){
				  fprintf(stderr, "Conn_input failed due to IO error:");
				  continue;

				  }else if(!r->state==RST_ESTABLISHED){
				  //No established connection yet!
				  printf("Connection not yet established!\n");
				  return;
				  }*/
			//Valid packet
			window_entry *window = (window_entry *)xmalloc(sizeof(window_entry));

			if(bytes_read<0) { // EOF reached
				packet_size = PKT_HEADER_SIZE;
				//EOF packet has no data but has seqno
				// TODO: fix this whole fucking method
				packet.seqno = htonl(r->next_seqno); r->next_seqno++;
				packet.len = htons(packet_size);
				packet.ackno=htonl(0);
				memset(&(packet.cksum),0,sizeof(uint16_t));
				packet.cksum=cksum((void*)&packet,PKT_HEADER_SIZE);
				//save packet in window entry
				memcpy(&window->pkt,&packet,sizeof(packet_t));
				window->valid=true;
				window->timeout = 0;
				r->sent_EOF = true;
			}
			else {
				//make a normal packet and add it to the window
				packet_size = PKT_HEADER_SIZE + bytes_read;
				// TODO: see above
				packet.seqno = htonl(r->next_seqno); r->next_seqno++;
				packet.len = htons(packet_size);
				packet.ackno=htonl(0);
				memset(&(packet.cksum),0,sizeof(uint16_t));
				packet.cksum=cksum((void*)&packet,packet_size); //TODO: I believe that the cksum should be over the whole packet, not just the header
				//save packet in window entry
				memcpy(&window->pkt,&packet,sizeof(packet_t));
				window->valid=true;
				window->timeout = 0;
			}
			//update window parameters
			r->lastSeqWritten = htonl(window->pkt.seqno);

			//send packet?
			conn_sendpkt(r->c, &window->pkt, packet_size);

			//Decode to host before enqueue
			window->pkt.len = ntohs (window->pkt.len);
			window->pkt.ackno = ntohl (window->pkt.ackno);
			window->pkt.seqno = ntohl(window->pkt.seqno);

			//enqueue
			windowList_enqueue(r, window, &r->sending_window);

			r->lastSeqSent = htonl(window->pkt.seqno);
			window_size = r->lastSeqWritten - r->lastSeqAcked;

		}
	}
}

void
rel_output (rel_t *r)
{
	window_entry *traverse = r->receiving_window;
	while(traverse != NULL) {
		if(!traverse->valid) {break;}
		else if(conn_bufspace(r->c) >= traverse->pkt.len - PKT_HEADER_SIZE){
			//commit the data
			if(!r->got_EOF){
				conn_output(r->c,(void*)(traverse->pkt.data),traverse->pkt.len - PKT_HEADER_SIZE);
				fprintf(stderr, "Out %d @ %d\n", traverse->pkt.seqno, getpid());
			}
			//was the pkt an EOF?
			if(traverse->pkt.len == PKT_HEADER_SIZE){
				//received an EOF packet
				r->got_EOF = true;
				if(r->sender_finished) rel_destroy(r);
				break;
			}



			traverse=traverse->next;
			r->nextSeqExpected++; //update the next expected sequence number

			window_entry *fetch = windowList_dequeue(r, &r->receiving_window); //slide the window - delete the newly written packet
			if(fetch != NULL){
				free(fetch);
			} else {fprintf(stderr, "we done fucked up\n");}
		}
	}

	//if there is nothing more in the receiving window and we have received an EOF packet, destroy
	//if(r->receiving_window==NULL && r->got_EOF) rel_destroy(r);
}

void
rel_timer ()
{
	/* Retransmit any packets that need to be retransmitted */
	//iterate throught the window and if an item is valid & it was transmitted > rel_t->cc->timeout milliseconds ago, then retransmit it.

	// TODO: fix this method, possibly with a different window
	rel_t *curr = rel_list;
	while(curr){
		window_entry *curr_win = rel_list->sending_window;
		while(curr_win){
			struct timespec currTime, diffTime;

			clock_gettime(CLOCK_MONOTONIC,&currTime);

			diffTime.tv_sec = currTime.tv_sec - curr_win->sen.tv_sec;
			diffTime.tv_nsec = currTime.tv_nsec - curr_win->sen.tv_nsec;
			fprintf(stderr, "pck %d | %d | %u | %d \n", curr_win->pkt.seqno, curr_win->valid, diffTime.tv_nsec, curr->cc->timeout);

			if(curr_win->valid && curr_win->timeout >=5){
				fprintf(stderr, "TIMEOUT! %d\n", curr_win->pkt.seqno);
				packet_t packet;

				memcpy(&packet, &curr_win->pkt, sizeof(packet_t));
				packet.len = htons(packet.len);
				packet.seqno = htonl(packet.seqno);
				packet.ackno = htonl(packet.ackno);
				curr_win->timeout = 0;
				//the packet is still valid (unacked) and has timed-out, retransmit
				clock_gettime(CLOCK_MONOTONIC,&(curr_win->sen)); //udpate the time sent
				conn_sendpkt(curr->c, &packet, curr_win->pkt.len); //send it
			}
			curr_win->timeout++;

			curr_win = curr_win->next;
		}
		curr = curr->next;
	}

}
