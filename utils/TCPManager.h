#ifndef TCPManager_h
#define TCPManager_h

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdio.h>
#include "TCPConstants.h"

#include <map>

#define BILLION 1000000000L


class TCPManager
{
public:
	int custom_recv(int sockfd, FILE* fp);
	int custom_send(int sockfd, FILE* fp, const struct sockaddr* remote_addr, socklen_t remote_addrlen);
	int custom_send_nobuffer(int sockfd, FILE* fp, const struct sockaddr* remote_addr, socklen_t remote_addrlen);

	TCPManager();
	~TCPManager();
private:
	uint16_t last_seq_num;
	uint16_t last_ack_num;

	uint16_t last_cumulative_seq_num;
	struct timespec last_received_msg_time;

	bool connection_established;

	uint16_t next_seq_num(int datalen);
	uint16_t next_ack_num(int datalen);

	int wait_for_packet();

	int timespec_subtract (struct timespec *result, struct timespec *y, struct timespec *x);
	void populateHeaders(void* buf, packet_headers &headers);

	bool compare_sockaddr(const struct sockaddr_in* sockaddr_1, const struct sockaddr_in* sockaddr_2);
	bool in_slow_start();
	void printMap();
	void copyHeaders(void* header, void* buffer);
	void copyData(void* header, void* buffer, int size);

	std::map<uint16_t, buffer_data> data_packets;
	
	uint16_t ssthresh;
	uint32_t cwnd;

};


#endif