#include "TCPManager.h"
#include "TCPConstants.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <iostream>

#include <stdlib.h>
#include <time.h>
#include <thread>

#include <string.h>	//mem_cpy()


TCPManager::TCPManager()
{
	srand(time(NULL)); //note that we must do this for our TCP ack/sequence numbers to be random
	last_seq_num = NOT_IN_USE;
	last_ack_num = NOT_IN_USE;
	connection_established = false;
}

TCPManager::~TCPManager()
{
}

/* 
 * Function: custom_accept(int sockfd, struct sockaddr *addr, socklen_t* addrlen, int flags)
 * Usage: custom_accept(sockfd, &addr, sizeof(addr), 0)
 * ------------------
 * This function blocks, listening for incoming SYNs and responds with a SYN-ACK.
 * After responding with a SYN-ACK, it remembers (connects) to the client.
 */
int TCPManager::custom_accept(int sockfd)
{
	char buf[MAX_PACKET_LENGTH];
	sockaddr_in client_addr;
	socklen_t client_addrlen = sizeof(client_addr);
	ssize_t count = recvfrom(sockfd, buf, MAX_PACKET_LENGTH, 0, (struct sockaddr *) &client_addr, &client_addrlen);
	if (count == -1) { 
		std::cerr << "recvfrom() ran into error" << std::endl;
		return -1;
	}
	else if (count > MAX_PACKET_LENGTH) {
		std::cerr << "Datagram too large for buffer" << std::endl;
		return -1;
	} /*
	else if (count != PACKET_HEADER_LENGTH) {
		std::cerr << "Unexpected bits when establishing connection" << std::endl;
		return -1;
	} */
	else {
		//Decompose the header data
		uint16_t seqnum = (buf[0] << 8 | buf[1]);
        uint16_t acknum = (buf[2] << 8 | buf[3]); //this should be 65535
        uint16_t winnum = (buf[4] << 8 | buf[5]);
        uint16_t flags  = (buf[6] << 8 | buf[7]);

        last_seq_num = (int) seqnum;
        last_ack_num = (int) acknum;
        
        if (!(flags ^ SYN_FLAG)) //check that ONLY the syn flag is set.
        {
        	std::cerr << "Received non-syn packet!" << std::endl;
        	return -1;
        }
		//Send SYN-ACK
		packet_headers synack_packet = {next_seq_num(0), next_ack_num(1), winnum, SYN_FLAG | ACK_FLAG};
		if (! sendto(sockfd, &synack_packet, PACKET_HEADER_LENGTH, 0, (struct sockaddr *) &client_addr, client_addrlen) ) {
			std::cerr << "Error: could not send synack_packet" << std::endl;
			return -1;
		}
	}
	
	return 0;
}

/**
 * Function: custom_connect(int sockfd, const struct sockaddr * addr, socklen_t addrlen)
 * Usage: custom_connect(sockfd, &addr, sizeof(addr))
 * -----------------------------
 * This function sends a UDP SYN message, and waits for the SYN-ACK to be received. 
 * It then sends a ACK. Sockfd is the socket you're sending over, and the sockaddr is the socket address you are sending to. 
 */
int TCPManager::custom_connect(int sockfd, const struct sockaddr * addr, socklen_t addrlen)
{

	packet_headers syn_packet = {next_seq_num(0), (uint16_t)NOT_IN_USE, INIT_RECV_WINDOW, SYN_FLAG};
	//send the initial syn packet
	if ( !sendto(sockfd, &syn_packet, PACKET_HEADER_LENGTH, 0, addr, addrlen) ) {
		std::cerr << "Error: Could not send syn_packet" << std::endl;
		return -1;
	}

	char buf[MAX_PACKET_LENGTH];
	sockaddr_in client_addr;
	socklen_t client_addrlen = sizeof(client_addr);
	
	clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);
	struct timespec result;
	bool message_received = false;
	while(!message_received)
	{
		do
		{
			struct timespec tmp;
			clock_gettime(CLOCK_MONOTONIC, &tmp);
			//wait for a response quietly.
			timespec_subtract(&result, &last_received_msg_time, &tmp);
			ssize_t count = recvfrom(sockfd, buf, MAX_PACKET_LENGTH, 0, (struct sockaddr *) &client_addr, &client_addrlen);
			if (count == -1) { 
				std::cerr << "recvfrom() ran into error" << std::endl;
				continue;
			}
			else if (count > MAX_PACKET_LENGTH) {
				std::cerr << "Datagram too large for buffer" << std::endl;
				continue;
			} 
			else
			{
				
			}
			std::cout << result.tv_nsec << std::endl;

		} while(result.tv_nsec > 50000000); //5 milliseconds = 50000000 nanoseconds
	}
	return 0;

}

int TCPManager::custom_recv(int sockfd, void* buf, size_t len, int flags)
{
	return -1;
}

int TCPManager::custom_send(int sockfd, void* buf, size_t len, int flags)
{
	
	return -1;
}


/**
 * Function: next_seq_num()
 * Usage: next_seq_num(datalen)
 * ------------------------
 * This function generates the next syn number: If it's uninitialized, it initializes it. 
 * Otherwise, it just returns what's stored. Upon receiving a packet one shoudl update last_syn_num
 * to the appropriate offset (based on byte length).
 * You pass in how big the packet you are sending is (datawise).
 * Note that syn/acks are one byte each.
 */

/*
uint16_t TCPManager::next_seq_num(int datalen)
{
	//generate the first seq number
	if(last_seq_num == -1)
		last_seq_num = rand() % MAX_SEQUENCE_NUMBER;
	
	int cached_seq_num = last_seq_num;
	last_seq_num += datalen;
	if(last_seq_num >= MAX_SEQUENCE_NUMBER) //this might overflow if you pass in too large a number for datalen
		last_seq_num -= MAX_SEQUENCE_NUMBER;
	return cached_seq_num;
}
*/

uint16_t TCPManager::next_seq_num(int datalen)
{
	//generate the first seq number
	if(last_seq_num == -1)
		last_seq_num = rand() % MAX_SEQUENCE_NUMBER;

	uint16_t next_seq_num = last_ack_num;
	last_ack_num += datalen;
	if (last_ack_num >= MAX_SEQUENCE_NUMBER)
			last_ack_num -= MAX_SEQUENCE_NUMBER;
	return next_seq_num;
}


/**
 * Function: next_ack_num()
 * Usage: next_ack_num(len)
 * -------------------------
 * This function generates the next ack number. last_ack_num should be set by the connection.
 * 
 */
 
 /*
uint16_t TCPManager::next_ack_num(int datalen)
{
	if(!connection_established)
		return -1;

	int cached_ack_num = last_ack_num;
	last_ack_num += datalen;
	if(last_ack_num >= MAX_SEQUENCE_NUMBER) //this might overflow if you pass in too large a number for datalen
		last_ack_num -= MAX_SEQUENCE_NUMBER;
	return cached_ack_num;
}
*/

// Next ack num = last sequence number received + amount of data received
uint16_t TCPManager::next_ack_num(int datalen)
{
	if(!connection_established)
		return -1;

	//Next ack number will be the recieved_seqNum + datalen
	next_ack_num = last_seq_num + datalen;
	if (next_ack_num >= MAX_SEQUENCE_NUMBER)
		next_ack_num -= MAX_SEQUENCE_NUMBER;

	return next_ack_num;
}


/* Subtract the ‘struct timeval’ values X and Y,
   storing the result in RESULT.
   Return 1 if the difference is negative, otherwise 0. */
   //Modified From http://www.gnu.org/software/libc/manual/html_node/Elapsed-Time.html
int TCPManager::timespec_subtract (struct timespec *result, struct timespec *y, struct timespec *x)
{
  /* Perform the carry for the later subtraction by updating y. */
  if (x->tv_nsec < y->tv_nsec) {
    int nsec = (y->tv_nsec - x->tv_nsec) / BILLION + 1;
    y->tv_nsec -= BILLION * nsec;
    y->tv_sec += nsec;
  }
  if (x->tv_nsec - y->tv_nsec > BILLION) {
    int nsec = (x->tv_nsec - y->tv_nsec) / BILLION;
    y->tv_nsec += BILLION * nsec;
    y->tv_sec -= nsec;
  }

  /* Compute the time remaining to wait.
     tv_nsec is certainly positive. */
  result->tv_sec = x->tv_sec - y->tv_sec;
  result->tv_nsec = x->tv_nsec - y->tv_nsec;

  /* Return 1 if result is negative. */
  return x->tv_sec < y->tv_sec;
}
