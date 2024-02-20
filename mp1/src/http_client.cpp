/*
** client.c -- a stream socket client demo
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <iostream>
#include <fstream>

#include <sys/time.h>
#include <chrono>


using namespace std;

#define PORT "3490" // the port client will be connecting to 

#define MAXDATASIZE 4096 // max number of bytes we can get at once 

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char *argv[])
{
	int sockfd, numbytes;  
	char buf[MAXDATASIZE];
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char s[INET6_ADDRSTRLEN];
    string addr, protocol, host, port, path;
	
	FILE *client_timestamp_fp;

	const int MAX_TIMESTAMP_COUNT = 100000; // Define the maximum number of timestamps you want to store
	long long timestamp_array[MAX_TIMESTAMP_COUNT];
	int timestamp_count = 0;

	int sleep_time = 10000; // sleep time in microseconds


	if (argc != 5) {
	    fprintf(stderr,"usage: client hostname filename_latency_timestamp filesize n_iterations\n");
	    exit(1);
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
    
    addr = argv[1];
	char *client_file_name = argv[2];
	long filesize = atoi(argv[3]);
	int n_iterations = atoi(argv[4]);
	long timestamp_filesize = filesize;
	client_timestamp_fp = fopen(client_file_name, "w");

    protocol = addr.substr(0, addr.find("//") - 1);
	addr = addr.substr(addr.find("//") + 2);
	if (addr.find('/') == addr.npos) path = "/";
	else path = addr.substr(addr.find('/'));
	host = addr.substr(0, addr.find('/'));
	if (host.find(':') != host.npos) {
		port = host.substr(host.find(':') + 1);
		host = host.substr(0, host.find(':'));
	}
	else port = "80";


	if ((rv = getaddrinfo(host.data(), port.data(), &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and connect to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("client: socket");
			continue;
		}

		if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("client: connect");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "client: failed to connect\n");
		return 2;
	}

	inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),
			s, sizeof s);
	
	// TODO: Remove this
	printf("client: connecting to %s\n", s);

	freeaddrinfo(servinfo); // all done with this structure
	
	// TODO: Remove this
	cout << host << ' ' << port << ' ' << path << endl;
	
    string request = "GET " + path + " HTTP/1.1\r\n" + "User-Agent: Wget/1.12(linux-gnu)\r\n" +
	  				 "Host: " + host + ":" + port + "\r\n" + "Connection: Keep-Alive\r\n\r\n";


	// ofstream out;
	// out.open("output", ios::binary);
	long total_bytes = 0;

	for (int j = 0; j < n_iterations; ++j) {
		usleep(sleep_time);
		struct timeval current_time;
		long long microseconds;


		// gettimeofday(&current_time, NULL);
		// microseconds = (long long)current_time.tv_sec * 1000000 + current_time.tv_usec;
		// timestamp_array[timestamp_count++] = microseconds;

		// Start timestamp
		auto start = std::chrono::high_resolution_clock::now();

		send(sockfd, request.c_str(), request.size(), 0);
		


		bool header = true;
		
		// printf("total bytes: %ld", total_bytes);
		


		while (true) {
			memset(buf, '\0', MAXDATASIZE);
			numbytes = recv(sockfd, buf, MAXDATASIZE, 0);
			buf[numbytes] = '\0';
			if (numbytes > 0) {
				// cout << numbytes << endl;
				total_bytes += numbytes;
				// if (header) {
				// 	// cout << buf << endl;
				// 	char* head = strstr(buf, "\r\n\r\n");
				// 	// cout << head << endl;
				// 	if (head != NULL){
				// 		head+=4;
				// 		header = false;
				// 	}
				// 	// out.write(head, numbytes - 19);
				// 	// printf("strlen(head): %ld", strlen(head) );
				// 	// printf("numbytes head %d", numbytes);
				// } 
				// else out.write(buf, sizeof(char) * numbytes);

				if (total_bytes - 19 >= timestamp_filesize) {
					// "TIMESTAMP" is in the received message
					// printf("printing timestamp\n");
					// gettimeofday(&current_time, NULL);
					// microseconds = (long long)current_time.tv_sec * 1000000 + current_time.tv_usec;
					// End timestamp
					auto end = std::chrono::high_resolution_clock::now();
					timestamp_array[timestamp_count++] = std::chrono::duration<double, std::micro>(end - start).count();
					timestamp_filesize += filesize;
					printf("total bytes: %ld \n", total_bytes);
					// printf("j: %d\n \n", j);
					fflush(stdout);
					break;
				} 
			} 
			else break;
		}
	}

	// gettimeofday(&current_time, NULL);
	// microseconds = (long long)current_time.tv_sec * 1000000 + current_time.tv_usec;
	// printf("%lld\n", microseconds);
	

	for (int i = 0; i < timestamp_count; ++i) {
		// if i is even, then print "start time" before the timestamp and if i is odd, then print 
		// "end time" before the timestamp
		// if (i % 2 == 0) fprintf(client_timestamp_fp, "start time: ");
		// else fprintf(client_timestamp_fp, "end time: ");
		fprintf(client_timestamp_fp, "%lld\n", timestamp_array[i]);
	}
	fclose(client_timestamp_fp);
	
	printf("total bytes: %ld", total_bytes);

	// if ((numbytes = recv(sockfd, buf, MAXDATASIZE-1, 0)) == -1) {
	//     perror("recv");
	//     exit(1);
	// }
	close(sockfd);

	// buf[numbytes] = '\0';

	// printf("client: received '%s'\n",buf);

	// out.close(); 

	return 0;
}

