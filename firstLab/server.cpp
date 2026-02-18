#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <cstring>

bool keepRunning = true;

void handleSignal(int signal) {
    keepRunning = false;
}

int main() {
	signal(SIGINT, handleSignal);
        signal(SIGTERM, handleSignal);

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0); // IPPROTOCOL_UDP
	sockaddr_in serverAddr, clientAddr;
	serverAddr.sin_port = htons(8080);
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = INADDR_ANY;

	bind(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr));

	char buffer[1024];
	while (keepRunning) {
	    socklen_t addrLen = sizeof(clientAddr);
	    memset(buffer, 0, sizeof(buffer));

	    int n = recvfrom(sockfd, buffer, 1024, 0, (struct sockaddr*)&clientAddr, &addrLen);
 	    buffer[n] = '\0';

	    char ip[INET_ADDRSTRLEN];
	    inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
	    int port = ntohs(clientAddr.sin_port);
	    std::cout << "Client" << ip << ";" << port << "->" << buffer << std::endl;
	    sendto(sockfd, buffer, n, 0, (struct sockaddr*)&clientAddr, addrLen);
	}
	close(sockfd);
	return 0;
}
