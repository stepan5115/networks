#include <iostream>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <csignal>
#include "structs.h"

#define PORT 8080
#define BUFFER_SIZE sizeof(Message)

bool keepRunning = true;

void handleSignal(int /*signal*/) {
    keepRunning = false;
}

void handle_client(int client_sock, struct sockaddr_in client_addr) {
    Message msg;
    char client_ip[INET_ADDRSTRLEN];
    int bytes_received;
    
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    std::cout << "Client connected" << std::endl;
    
    bytes_received = recv(client_sock, &msg, sizeof(Message), 0);
    if (bytes_received <= 0 || msg.type != MSG_HELLO) {
        std::cout << "Failed to receive HELLO" << std::endl;
        return;
    }
    
    std::cout << "[" << client_ip << ":" << ntohs(client_addr.sin_port) 
              << "]: Hello (" << msg.payload << ")" << std::endl;
    
    msg.type = MSG_WELCOME;
    snprintf(msg.payload, MAX_PAYLOAD, "Welcome %s:%d", client_ip, ntohs(client_addr.sin_port));
    msg.length = htonl(sizeof(msg.type) + strlen(msg.payload) + 1);
    
    if (send(client_sock, &msg, sizeof(Message), 0) < 0) {
        perror("send");
        return;
    }
    
    while (keepRunning) {
        memset(&msg, 0, sizeof(Message));
        bytes_received = recv(client_sock, &msg, sizeof(Message), 0);
        
        if (bytes_received <= 0) {
            std::cout << "Client disconnected" << std::endl;
            break;
        }
        
        switch (msg.type) {
            case MSG_TEXT:
                std::cout << "[" << client_ip << ":" << ntohs(client_addr.sin_port) 
                          << "]: " << msg.payload << std::endl;
                break;
                
            case MSG_PING:
                std::cout << "Received PING, sending PONG" << std::endl;
                msg.type = MSG_PONG;
                strcpy(msg.payload, "PONG");
                msg.length = htonl(sizeof(msg.type) + strlen(msg.payload) + 1);
                send(client_sock, &msg, sizeof(Message), 0);
                break;
                
            case MSG_BYE:
                std::cout << "Client requested to disconnect" << std::endl;
                close(client_sock);
                return;
                
            default:
                std::cout << "Unknown message type: " << msg.type << std::endl;
        }
    }
    
    close(client_sock);
}

int main() {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        exit(1);
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_sock);
        exit(1);
    }
    
    if (listen(server_sock, 1) < 0) {
        perror("listen");
        close(server_sock);
        exit(1);
    }
    
    std::cout << "Server listening on port " << PORT << std::endl;
    
    client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
    if (client_sock < 0) {
        perror("accept");
        close(server_sock);
        exit(1);
    }
    
    handle_client(client_sock, client_addr);
    
    close(server_sock);
    std::cout << "Server shutting down" << std::endl;
    return 0;
}