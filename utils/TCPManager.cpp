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

#include <arpa/inet.h>

#include <map>
TCPManager::TCPManager()
{
	last_seq_num = NOT_IN_USE;
	last_ack_num = NOT_IN_USE;
    last_cumulative_seq_num = NOT_IN_USE;
	connection_established = false;
    cwnd = INIT_WINDOW_SIZE;
    ssthresh = cwnd / 2;

}

TCPManager::~TCPManager()
{
}

/* 
 * Function: custom_recv(int sockfd, FILE* fp)
 * Usage: custom_recv(sockfd,fp)
 * ------------------
 * sockfd is the socket to listen upon, fp is the FILE* that the server should be serve
 * This function blocks, listening for incoming SYNs and responds with a SYN-ACK.
 *
 * This function handles the entire server side of the code, taking a socket to listen upon and responding over the file pointer.
 * It listens for Syns, and replies with Syn-acks to the sender adddress, establishing a TCP connection, and upon the receiving of
 * and ack it starts to send the window data back with the congestion control targetted appropriately. 
 */
int TCPManager::custom_recv(int sockfd, FILE* fp)
{
    char buf[MAX_PACKET_LENGTH+1]; //will end up null terminated.

    //The client we're connecting to: we remember this for the rest of the connection status
    //If we receive packets from someone that isn't this address, we just drop them. 
    sockaddr_in client_addr;
    socklen_t client_addrlen = sizeof(client_addr);

    sockaddr_in received_addr;
    socklen_t received_addrlen = sizeof(received_addr);

    bool ack_received = false, syn_received = false, fin_established = false, file_complete = false;
    //Wait for someone to establish a connection through SYN. Ignore all other packets. 
    
    //wait for a packet
    //Note that the received data is HTONS already here, as blocking does so, and recvfrom is stupid
    while(!syn_received)
    {
        ssize_t count = recvfrom(sockfd, buf, MAX_PACKET_LENGTH, 0, (struct sockaddr *) &client_addr, &client_addrlen);

        if (count == -1) { 
            perror("recvfrom");
            std::cerr << "recvfrom() ran into error" << std::endl;
        }
        else if (count > MAX_PACKET_LENGTH)
            std::cerr << "Datagram too large for buffer" << std::endl;
        else {
            //Decompose the header data
            struct packet_headers received_packet_headers;
            populateHeaders(buf, received_packet_headers);
            
            last_seq_num = (received_packet_headers.h_seq);
            last_ack_num = (received_packet_headers.h_ack);
            
            if (!((received_packet_headers.flags) ^ SYN_FLAG)) //check that ONLY the syn flag is set.
            {
                std::cout << "Receiving SYN packet" << std::endl;
                syn_received = true;
            }
        }
    }


    //Send initial SYN-ACK, set timeout.
    packet_headers synack_packet = {next_seq_num(0), next_ack_num(1), cwnd, SYN_FLAG | ACK_FLAG};
    
    struct timespec result;
    
    if (!sendto(sockfd, &synack_packet, PACKET_HEADER_LENGTH, 0, (struct sockaddr *) &client_addr, client_addrlen) ) {
        std::cerr << "Error: could not send synack_packet" << std::endl;
        return -1;
    }
    else
        std::cout << "Sending SYN-ACK " << synack_packet.h_ack << std::endl;

    //begin the timeout
    clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);
    long bits_in_transit;
    // Wait for ACK, timeout to SYN-ACK
    while(!ack_received)
    {
        do
        {
            struct timespec tmp;
            clock_gettime(CLOCK_MONOTONIC, &tmp);
            //wait for a response quietly.
            client_addrlen = sizeof(client_addr);
            timespec_subtract(&result, &last_received_msg_time, &tmp);
            ssize_t count = recvfrom(sockfd, buf, MAX_PACKET_LENGTH, MSG_DONTWAIT, (struct sockaddr *) &received_addr, &received_addrlen); //non-blocking
            
            if(count == -1 && errno == EAGAIN)
            {
                continue;
            }
            else if (count == -1) { 
                std::cerr << "recvfrom() ran into error" << std::endl;
                continue;
            }
            else if(!compare_sockaddr(&client_addr, &received_addr))
            {
                //different source
                continue;
            }
            else if (count > MAX_PACKET_LENGTH) {
                std::cerr << "Datagram too large for buffer" << std::endl;
                continue;
            } 
            else
            {
                struct packet_headers received_packet_headers;
                populateHeaders(buf, received_packet_headers);

                last_seq_num = received_packet_headers.h_seq;
                last_ack_num = received_packet_headers.h_ack;
                bits_in_transit = received_packet_headers.h_window;

                if (!(received_packet_headers.flags ^ (ACK_FLAG))) 
                {
                    ack_received = true;
                    std::cout << "Receiving ACK " << last_ack_num << std::endl;
                    break;
                }
                else if(!(received_packet_headers.flags ^ SYN_FLAG)) //SYN-ACK lost, and another SYN received. resend syn-ack.
                {
                    if ( !sendto(sockfd, &synack_packet, PACKET_HEADER_LENGTH, 0, (struct sockaddr *) &client_addr, client_addrlen) )  {
                        std::cerr << "Error: Could not send syn_packet" << std::endl;
                        return -1;
                    }
                    else
                    {
                        std::cout << "Sending SYN-ACK " << synack_packet.h_ack << " Retransmission" << std::endl;
                    }
                    clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);
                }
            }

        } while(result.tv_nsec < 500000000); //500 milliseconds = 500000000 nanoseconds

        if(!ack_received)
        {
            if (! sendto(sockfd, &synack_packet, PACKET_HEADER_LENGTH, 0, (struct sockaddr *) &client_addr, client_addrlen) ) {
                std::cerr << "Error: could not send synack_packet" << std::endl;
                return -1;
            }
            else
            {
                std::cout << "Sending SYN-ACK " << synack_packet.h_ack <<  " Retransmission" << std::endl;
            }
            clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);
        }

    }

    uint16_t seqnum = last_ack_num;
    uint16_t acknum = last_seq_num + 1;
    if(acknum > MAX_SEQUENCE_NUMBER)
        acknum -= MAX_SEQUENCE_NUMBER;

    
    //Connection established, can begin sending data, according to window size.
    while( !data_packets.empty() || !file_complete) //three conditions to end server: client unexpected end, out of file data to send
    {
        struct timespec tmp;
        clock_gettime(CLOCK_MONOTONIC, &tmp);
        timespec_subtract(&result, &last_received_msg_time, &tmp);
        
        //check for timeout
        if(result.tv_nsec > 500000000)
        {
            //update window sizes
            if(in_slow_start())
            {
                ssthresh = cwnd/2;
                cwnd = MAX_PACKET_PAYLOAD_LENGTH;
            }
            else
            {
                cwnd = cwnd/2;
            }

            //resend the entire window
            for(auto it : data_packets) //automatically iterate over the map
            {
                //extract the packet headers.
                uint16_t sequence_num = it.second.data[0] << 8 | it.second.data[1];

                if ( !sendto(sockfd, &it.second.data, it.second.size + 8, 0, (struct sockaddr*) &client_addr, client_addrlen) )  {
                    std::cerr << "Error: Could not retrasmit data_packet" << std::endl;
                    return -1;
                }
                else
                {
                    std::cout << "Sending data packet " <<  sequence_num << " " << cwnd << " " << ssthresh << " Retransmission" << std::endl;
                }
            }

            //reset the timeout window
            clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);
        }

        while(bits_in_transit < cwnd && !file_complete && bits_in_transit < last_recv_window)
        {
            buffer_data b;
            //read the data from the disk
            int readnum = fread(b.data+8, sizeof(char), 1024,  fp);
            b.size = readnum + 8;

            //note that acknum never increases because we never get data from the client.
            packet_headers p = {seqnum, acknum, INIT_RECV_WINDOW, 0};
            copyHeaders(&p, &b.data);
            seqnum += readnum;
            if(seqnum > MAX_SEQUENCE_NUMBER)
                seqnum -= MAX_SEQUENCE_NUMBER;

            if(readnum < 1024)
                file_complete = true;  

            //send more packets!
            if ( !sendto(sockfd, b.data, readnum + 8, 0, (struct sockaddr*) &client_addr, client_addrlen) )  {
                std::cerr << "Error: Could not retrasmit data_packet" << std::endl;
                return -1;
            }
            else
            {
                std::cout << "Sending data packet " <<  p.h_seq << " " << cwnd << " " << ssthresh << std::endl;
            }

            data_packets.insert(std::pair<uint16_t, buffer_data>(p.h_seq + b.size - 8, b));
            
        }
        
        //check to see if we've received any ACKs
        ssize_t count = recvfrom(sockfd, buf, MAX_PACKET_LENGTH, MSG_DONTWAIT, (struct sockaddr *) &received_addr, &received_addrlen); //non-blocking
        if(count == -1 && errno == EAGAIN)
        {
            //no data received
            continue;
        }
        else if (count == -1) { 
            std::cerr << "recvfrom() ran into error" << std::endl;
            continue;
        }
        else if (count > MAX_PACKET_LENGTH) {
            std::cerr << "Datagram too large for buffer" << std::endl;
            continue;
        } 
        else if(!compare_sockaddr(&client_addr, &received_addr))
        {
            //different source, ignore
            continue;
        }   

        //received a packet, should process it.
        packet_headers received_packet_headers;                
        populateHeaders(buf, received_packet_headers);
        bits_in_transit = received_packet_headers.h_window;
        last_seq_num = received_packet_headers.h_seq;
        last_ack_num = received_packet_headers.h_ack;

        switch(received_packet_headers.flags)
        {
            case ACK_FLAG:  //ACKing a prior message
                //remove that packet from the mapping
                //note that we don't particularly care if the ack was received for an imaginary packet
                std::cout << "Receiving ACK Packet " << received_packet_headers.h_ack;
                if(data_packets.count(received_packet_headers.h_ack) > 0) //packet has been received, we remove it
                {
                    data_packets.erase(received_packet_headers.h_ack);
                }
                else
                {
                    std::cout << " Retransmission";
                }
                std::cout << std::endl;

                //ack successfully received
                cwnd = in_slow_start() ? cwnd * 2 : cwnd + MAX_PACKET_PAYLOAD_LENGTH;
                break;
            default: //Unknown
                break;
        }

    }
    
    
    if (file_complete) {
        //TODO: fix these seq and ack numbers
        clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);
        packet_headers fin_packet = {(uint16_t)NOT_IN_USE, (uint16_t)NOT_IN_USE, cwnd, FIN_FLAG};
        //send the inital FIN packet
        if ( !sendto(sockfd, &fin_packet, PACKET_HEADER_LENGTH, 0, remote_addr, remote_addrlen) )  {
            std::cerr << "Error: Could not send fin_packet" << std::endl;
            return -1;
        }
        else
        {
            std::cout << "Sending FIN" << std::endl;
        }
    }
    else
        std::cerr << "Should not have gotten here. File should have been completely sent\n" std::endl;
    
    while (!fin_established) {
		do
		{
			clock_gettime(CLOCK_MONOTONIC, &tmp);
			//wait for a response quietly.
			timespec_subtract(&result, &last_received_msg_time, &tmp);
			ssize_t count = recvfrom(sockfd, buf, MAX_PACKET_LENGTH, MSG_DONTWAIT, (struct sockaddr *) &client_addr, &client_addrlen); //non-blocking
            if(count == -1 && errno == EAGAIN)
            {
                //no data received
                continue;
            }
			else if (count == -1) { 
				std::cerr << "recvfrom() ran into error" << std::endl;
				continue;
			}
            else if(!compare_sockaddr(&client_addr, (sockaddr_in *)remote_addr))
            {
                //different source
                continue;
            }
			else if (count > MAX_PACKET_LENGTH) {
				std::cerr << "Datagram too large for buffer" << std::endl;
				continue;
			} 
			else
			{

                struct packet_headers received_packet_headers;                
                populateHeaders(buf, received_packet_headers);

                last_seq_num = received_packet_headers.h_seq;
                last_ack_num = received_packet_headers.h_ack;

				if (!(received_packet_headers.flags ^ (FIN_FLAG | ACK_FLAG))) 
				{
					fin_established = true;
                    std::cout << "Receiving FIN_ACK " << last_ack_num << std::endl;
					break;
				}
                else if(!(received_packet_headers.flags ^ ACK_FLAG)) //FIN lost, and another regular ACK received. Resend FIN.
                {
                    if ( !sendto(sockfd, &fin_packet, PACKET_HEADER_LENGTH, 0, (struct sockaddr *) &remote_addr, remote_addrlen) )  {
                        std::cerr << "Error: Could not send fin_packet" << std::endl;
                        return -1;
                    }
                    else
                    {
                        std::cout << "Sending FIN " << fin_packet.h_ack << " Retransmission" << std::endl;
                    }
                    clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);
                }
			// std::cerr << result.tv_nsec << std::endl;
            }
		} while(result.tv_nsec < 500000000); //5 milliseconds = 50000000 nanoseconds
        
        if(!fin_established)
        {
            //Resend the fin packet
            if ( !sendto(sockfd, &fin_packet, PACKET_HEADER_LENGTH, 0, remote_addr, remote_addrlen) )  {
                std::cerr << "Error: Could not send fin_packet" << std::endl;
                return -1;
            }
            else
            {
                std::cout << "Sending FIN Retransmission" << std::endl;
            }

            clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);
        }
        
    }
    
    //TODO: fix these seq and ack numbers
    //TODO: Start timer after sending final ack
    clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);
    packet_headers final_ack_packet = {(uint16_t)NOT_IN_USE, (uint16_t)NOT_IN_USE, cwnd, ACK_FLAG};
    //send the final ACK packet
    do {
        if ( !sendto(sockfd, &final_ack_packet, PACKET_HEADER_LENGTH, 0, remote_addr, remote_addrlen) )  {
            std::cerr << "Error: Could not send final_ack_packet" << std::endl;
            return -1;
        }
        else
        {
            std::cout << "Sending ACK" << std::endl;
        }

        clock_gettime(CLOCK_MONOTONIC, &tmp);
		timespec_subtract(&result, &last_received_msg_time, &tmp);
		ssize_t count = recvfrom(sockfd, buf, MAX_PACKET_LENGTH, 0, (struct sockaddr *) &client_addr, &client_addrlen); //blocking
        
        if (count > 0)
            clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);

    } while (result.tv_nsec < 500000000);
    return 0;
}

/**
 * Function: custom_send(int sockfd, FILE* fp, const struct *sockaddr *remote_addr, socklen_t remote_addrlen
 * Usage: custom_send(sockfd, fp, &remote_addr, remote_addrlen)
 * -----------------------------
 * sockfd is the socket to send the request out upon/receive the request on; fp is the file to save the results to, remote_addr 
 * is the remote address for the socket, remote_addrlen is the length of that sturct
 * 
 * This function manages the entire client side of the code: sending out a SYN, waiting for a SYN-ACK and replying with the ACK and
 * receiving data. Also handles the closing of the connection: it blocks. 
 */
int TCPManager::custom_send(int sockfd, FILE* fp, const struct sockaddr *remote_addr, socklen_t remote_addrlen)
{

	packet_headers syn_packet = {next_seq_num(0), (uint16_t)NOT_IN_USE, cwnd, SYN_FLAG};

	char buf[MAX_PACKET_LENGTH];

	sockaddr_in client_addr;
	socklen_t client_addrlen = sizeof(client_addr);

	struct timespec result;
	bool message_received = false;

    //send the inital syn packet
    if ( !sendto(sockfd, &syn_packet, PACKET_HEADER_LENGTH, 0, remote_addr, remote_addrlen) )  {
        std::cerr << "Error: Could not send syn_packet" << std::endl;
        return -1;
    }
    else
    {
        std::cout << "Sending SYN" << std::endl;
    }

    clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);

	while(!message_received)
	{
		do
		{
			struct timespec tmp;
			clock_gettime(CLOCK_MONOTONIC, &tmp);
			//wait for a response quietly.
			timespec_subtract(&result, &last_received_msg_time, &tmp);
			ssize_t count = recvfrom(sockfd, buf, MAX_PACKET_LENGTH, MSG_DONTWAIT, (struct sockaddr *) &client_addr, &client_addrlen); //non-blocking
            if(count == -1 && errno == EAGAIN)
            {
                //no data received
                continue;
            }
			else if (count == -1) { 
				std::cerr << "recvfrom() ran into error" << std::endl;
				continue;
			}
            else if(!compare_sockaddr(&client_addr, (sockaddr_in *)remote_addr))
            {
                //different source
                continue;
            }
			else if (count > MAX_PACKET_LENGTH) {
				std::cerr << "Datagram too large for buffer" << std::endl;
				continue;
			} 
			else
			{

                struct packet_headers received_packet_headers;                
                populateHeaders(buf, received_packet_headers);

                last_seq_num = received_packet_headers.h_seq;
                last_ack_num = received_packet_headers.h_ack;

				if (!(received_packet_headers.flags ^ (ACK_FLAG | SYN_FLAG))) 
				{
					message_received = true;
                    std::cout << "Receiving SYN-ACK " << last_ack_num << std::endl;
					break;
				}
			// std::cerr << result.tv_nsec << std::endl;
            }
		} while(result.tv_nsec < 500000000); //5 milliseconds = 50000000 nanoseconds

        if(!message_received)
        {
            //send the inital syn packet
            if ( !sendto(sockfd, &syn_packet, PACKET_HEADER_LENGTH, 0, remote_addr, remote_addrlen) )  {
                std::cerr << "Error: Could not send syn_packet" << std::endl;
                return -1;
            }
            else
            {
                std::cout << "Sending SYN Retransmission" << std::endl;
            }

            clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);
        }
	}

	//Send ACK-Packet
	packet_headers ack_packet = {next_seq_num(0), next_ack_num(1), INIT_RECV_WINDOW, ACK_FLAG};
	if ( !sendto(sockfd, &ack_packet, PACKET_HEADER_LENGTH, 0, remote_addr, remote_addrlen) ) {
		std::cerr << "Error: Could not send ack_packet" << std::endl;
		return -1;
	}
    else
    {
        std::cout << "Sending ACK " << ack_packet.h_ack << std::endl;
    }

    //Now we set up the connection data transfer, and wait for a fin
    //note that last_received_msg_time is being repurposed to the oldest packet's time.
    
    bool fin_established = false;
    while(!fin_established || !data_packets.empty())
    {
        struct timespec tmp;
        clock_gettime(CLOCK_MONOTONIC, &tmp);
        timespec_subtract(&result, &last_received_msg_time, &tmp);
        
        //check for timeout
        if(result.tv_nsec > 500000000)
        {

            if(in_slow_start())
            {
                ssthresh = cwnd/2;
                cwnd = MAX_PACKET_PAYLOAD_LENGTH;
            }
            else
            {
                cwnd = cwnd/2;
            }

            //resend the entire window
            for(auto it : data_packets) //automatically iterate over the map
            {
                //extract the packet headers.
                uint16_t ack_num = it.second.data[2] << 8 | it.second.data[3];

                if ( !sendto(sockfd, &it.second.data, it.second.size, 0, remote_addr, remote_addrlen) )  {
                    std::cerr << "Error: Could not retrasmit data_packet" << std::endl;
                    return -1;
                }
                else
                {
                    std::cout << "Sending ACK packet " <<  ack_num << " Retransmission" << std::endl;
                }
            }

            clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);

            //update the window sizes.

        }

        ssize_t count = recvfrom(sockfd, buf, MAX_PACKET_LENGTH, MSG_DONTWAIT, (struct sockaddr *) &client_addr, &client_addrlen); //non-blocking

        if(count == -1 && errno == EAGAIN)
        {
            //no data received
            continue;
        }
        else if (count == -1) { 
            std::cerr << "recvfrom() ran into error" << std::endl;
            continue;
        }
        else if (count > MAX_PACKET_LENGTH) {
            std::cerr << "Datagram too large for buffer" << std::endl;
            continue;
        } 
        else if(!compare_sockaddr(&client_addr, (sockaddr_in *) remote_addr))
        {
            //different source, ignore
            continue;
        }   

        //received a packet, should process it.
        struct packet_headers received_packet_headers;                
        populateHeaders(buf, received_packet_headers);


        switch(received_packet_headers.flags)
        {
            case ACK_FLAG | SYN_FLAG: //ACK_SYN, resubmit. We don't actually track in our window, so we inflate slightly slower!
                if ( !sendto(sockfd, &ack_packet, PACKET_HEADER_LENGTH, 0, remote_addr, remote_addrlen) ) {
                    std::cerr << "Error: Could not send ack_packet" << std::endl;
                    return -1;
                }
                else
                {
                    std::cout << "Sending ACK " << ack_packet.h_ack << " Retransmission" << std::endl;
                }
                break;
            
            case FIN_FLAG:  //FIN flag. Send back a FIN_ACK
                //TODO: fix these seq and ack numbers
                packet_headers finack_packet = {(uint16_t)NOT_IN_USE, (uint16_t)NOT_IN_USE, cwnd, FIN_FLAG | ACK_FLAG};
                if ( !sendto(sockfd, &finack_packet, PACKET_HEADER_LENGTH, 0, remote_addr, remote_addrlen) ) {
                    std::cerr << "Error: Could not send finack_packet" << std::endl;
                    return -1;
                }
                else
                {
                    std::cout << "Sending FIN_ACK " << std::endl;
                }
                fin_established = true;
                break;
            case FIN_FLAG | ACK_FLAG: //FIN_ACK established. This should never happen (our window deflates before we send a FIN/FIN-ACK)
                break;
            case ACK_FLAG:  
            default: 
                //Should be a data packet, we don't care about an ACK or to our data messages
                //This is because the client would time out and send the appropriate data back. 
                //remove that packet from the mapping of our window
                //note that we don't particularly care if the ack was received for an imaginary packet
                std::cout << "Receiving data packet " << received_packet_headers.h_ack;
                if(data_packets.count(received_packet_headers.h_ack) > 0) //packet has been received, we remove it
                {
                    data_packets.erase(received_packet_headers.h_ack);
                    
                    //write the packet to the file: we're promised in order with this implementation
                    fwrite(buf, sizeof(char), count - 8, fp);
                    clock_gettime(CLOCK_MONOTONIC, &last_received_msg_time);

                }
                else //we already received this one.
                {
                    std::cout << " Retransmission";
                }
                std::cout << std::endl;

                if (fin_established)
                    break;
                
                cwnd = in_slow_start() ? cwnd * 2 : cwnd + MAX_PACKET_PAYLOAD_LENGTH;

                while(data_packets.size() < cwnd)
                {
                    //send more request packets out
                    //TODO: Fix flag counts
                    struct packet_headers p = {next_seq_num(count), next_ack_num(count), INIT_RECV_WINDOW, ACK_FLAG}; 
                    struct buffer_data d;
                    copyHeaders(&p, &d.data);
                    d.size = 8;

                    if ( !sendto(sockfd, d.data, d.size, 0, remote_addr, remote_addrlen) )  {
                        std::cerr << "Error: Could not retrasmit ack_packet" << std::endl;
                        return -1;
                    }
                    else
                    {
                        std::cout << "Sending ACK packet " <<  p.h_ack << " Retransmission" << std::endl;
                    }

                    data_packets.insert(std::pair<uint16_t, buffer_data>(p.h_seq, d));
                }


                break;
        }

    }
    return 0;

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

uint16_t TCPManager::next_seq_num(int datalen)
{
	//generate the first seq number, when no ack has yet been received. 
	if(last_ack_num == NOT_IN_USE)
    {
        last_cumulative_seq_num = rand() % MAX_SEQUENCE_NUMBER;
		return last_cumulative_seq_num;
    }

    //sequence numbers are cumulative: once you've sent the data the sequence number will go up
	uint16_t cache_seq_num = last_ack_num;
	cache_seq_num += datalen;
	if (cache_seq_num >= MAX_SEQUENCE_NUMBER)
			cache_seq_num -= MAX_SEQUENCE_NUMBER;
    if(last_cumulative_seq_num >= cache_seq_num)
        cache_seq_num += last_cumulative_seq_num; //acks are accumulative, so we should store the amount of data. 
    last_cumulative_seq_num = cache_seq_num;
	return last_cumulative_seq_num;
}


/**
 * Function: next_ack_num()
 * Usage: next_ack_num(len)
 * -------------------------
 * This function generates the next ack number. last_seq_num should be set by the connection, otherwise it returns an error. 
 * 
 */
// Next ack num = last sequence number received + amount of data received
uint16_t TCPManager::next_ack_num(int datalen)
{
    if(last_seq_num == NOT_IN_USE)
        return -1; //error, this function should not yet be called.
                    //acks are only cumulative based on the data that has been received: that is, the data that has been returned in sequence.

	//Next ack number will be the recieved_seqNum + datalen
	uint16_t next_ack_num = last_seq_num + datalen;
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


/* 
 * Function: populateHeaders(void* buf, packet_headers &headers)
 * Usage: populateHeaders(buf, headers)
 * --------------------------------
 * This function receives a buffer (that should be acquired from recvfrom, with minimum 8 bytes), taking the first 8 bytes 
 * and casting them to the struct. This will parse the headers and store them into the struct that's passed in.
 * 
 * WARNING: This does not safety checking: void*buf MUST be long enough as well and valid as well as headers must exist.
 */
void TCPManager::populateHeaders(void* buf, packet_headers &headers)
{
	char* buff = (char *) buf;
	headers.h_seq    = htons(buff[0] << 8 | buff[1]);
    headers.h_ack    = htons(buff[2] << 8 | buff[3]); 
    headers.h_window = htons(buff[4] << 8 | buff[5]);
    headers.flags    = htons(buff[6] << 8 | buff[7]); 
}

/*
 * Returns true if the passed in sockaddresses have the same port, address, and family.
 */
bool TCPManager::compare_sockaddr(const struct sockaddr_in* sockaddr_1, const struct sockaddr_in* sockaddr_2)
{
    return sockaddr_1->sin_port == sockaddr_2->sin_port  //same port number
            && sockaddr_1->sin_addr.s_addr == sockaddr_2->sin_addr.s_addr //same source address
            && sockaddr_1->sin_family == sockaddr_2->sin_family;
}

bool TCPManager::in_slow_start()
{
    return cwnd < ssthresh;
}

void TCPManager::copyHeaders(void* header, void* buffer)
{
    for(int i = 0; i < 8; i++)
    {
        ((char *)buffer)[i] = ((char *)header)[i];
    }
}

void TCPManager::copyData(void* header, void* buffer, int size)
{
    for(int i = 8; i < size+8; i++)
    {
        ((char *)buffer)[i] = ((char *)header)[i];
    }
}