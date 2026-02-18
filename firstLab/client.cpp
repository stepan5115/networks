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

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    char buffer[1024];
    std::string message;
    while (keepRunning) {
        std::cout << "> ";
        std::getline(std::cin, message);
	if (!keepRunning) break;
	sendto(sockfd, message.c_str(), message.length(), 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
	memset(buffer, 0, sizeof(buffer));

        recvfrom(sockfd, buffer, 1024, 0, NULL, NULL);
        std::cout << "Server: " << buffer << std::endl;
    }
    close(sockfd);
    return 0;
}
