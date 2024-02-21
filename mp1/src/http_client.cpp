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
#define PORT "9000" // the port client will be connecting to 

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
	int n_iterations = 1000;
	int total_bytes = 0;

    if (argc != 2) {
        fprintf(stderr,"usage: client hostname\n");
        exit(1);
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(argv[1], PORT, &hints, &servinfo)) != 0) {
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
    printf("client: connecting to %s\n", s);

    freeaddrinfo(servinfo); // all done with this structure


	string request = "1048575";
	int datasize = 1048575;
	int data_tracking = datasize;
	const int MAX_TIMESTAMP_COUNT = 100000; // Define the maximum number of timestamps you want to store
	long long timestamp_array[MAX_TIMESTAMP_COUNT];
	int timestamp_count = 0;

	for (int i = 0; i < n_iterations; i++) {

		// printf("was here\n");
		// usleep(10000); // 1ms
		auto start = std::chrono::high_resolution_clock::now();
		send(sockfd, request.c_str(), request.size(), 0);

		

		while (true){
			memset(buf, '\0', MAXDATASIZE); // TODO: dubious
			if ((numbytes = recv(sockfd, buf, MAXDATASIZE-1, 0)) == -1) {
				perror("recv");
				exit(1);
			}
			if (numbytes > 0) {
				total_bytes += numbytes;
				if (total_bytes == data_tracking) {
					printf("total_bytes: %d\n", total_bytes);
					data_tracking += datasize;
					auto end = std::chrono::high_resolution_clock::now();
					// printf("%ld\n", std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
					timestamp_array[timestamp_count++] =
						std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
					break;
				}
			} else {
				break;
			}
		}
		

	buf[numbytes] = '\0';

    // printf("client: received '%s'\n",buf);

	}

    FILE *client_timestamp_fp;
	const char *client_file_name = "client_ts.csv";
	client_timestamp_fp = fopen(client_file_name, "w");
    for (int i = 0; i < timestamp_count; ++i) {
		// if i is even, then print "start time" before the timestamp and if i is odd, then print 
		// "end time" before the timestamp
		// if (i % 2 == 0) fprintf(client_timestamp_fp, "start time: ");
		// else fprintf(client_timestamp_fp, "end time: ");
		fprintf(client_timestamp_fp, "%lld\n", timestamp_array[i]);
	}
	fclose(client_timestamp_fp);

    close(sockfd);

    return 0;
}