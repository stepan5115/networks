#include <iostream>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <csignal>
#include "structs.h"

#define SERVER_PORT 8080
#define SERVER_IP "127.0.0.1"
#define NICKNAME "CHELOVEK PAUK"

bool keepRunning = true;

void handleSignal(int /*signal*/) {
    keepRunning = false;
}

bool handshake_with_server(int sock) {
    Message msg;
    
    memset(&msg, 0, sizeof(Message));
    msg.type = MSG_HELLO;
    strncpy(msg.payload, NICKNAME, MAX_PAYLOAD - 1);
    msg.length = htonl(sizeof(msg.type) + strlen(msg.payload) + 1);
    
    if (send(sock, &msg, sizeof(Message), 0) < 0) {
        perror("send");
        return false;
    }
    
    memset(&msg, 0, sizeof(Message));
    if (recv(sock, &msg, sizeof(Message), 0) <= 0) {
        perror("recv");
        return false;
    }
    
    if (msg.type == MSG_WELCOME) {
        std::cout << msg.payload << std::endl;
        return true;
    } else {
        std::cout << "Unexpected message type: " << msg.type << std::endl;
        return false;
    }
}

void prepare_message(Message* msg, const char* input) {
    memset(msg, 0, sizeof(Message));
    
    if (strcmp(input, "/quit") == 0) {
        msg->type = MSG_BYE;
        strcpy(msg->payload, "bye");
    } else if (strcmp(input, "/ping") == 0) {
        msg->type = MSG_PING;
        strcpy(msg->payload, "ping");
    } else {
        msg->type = MSG_TEXT;
        strncpy(msg->payload, input, MAX_PAYLOAD - 1);
    }
    
    msg->length = htonl(sizeof(msg->type) + strlen(msg->payload) + 1);
}

bool handle_server_response(int sock) {
    Message msg;
    
    memset(&msg, 0, sizeof(Message));
    int bytes_received = recv(sock, &msg, sizeof(Message), 0);
    
    if (bytes_received <= 0) {
        std::cout << "Server disconnected" << std::endl;
        return false;
    }
    
    switch (msg.type) {
        case MSG_PONG:
            std::cout << "PONG" << std::endl;
            break;
            
        case MSG_BYE:
            std::cout << "Server closed connection" << std::endl;
            return false;
            
        default:
            std::cout << "Unknown message type: " << msg.type << std::endl;
    }
    
    return true;
}

void communication_loop(int sock) {
    char input[MAX_PAYLOAD];
    Message msg;
    
    while (keepRunning) {
        std::cout << "> ";
        
        if (fgets(input, MAX_PAYLOAD, stdin) == NULL) {
            break;
        }
        input[strcspn(input, "\n")] = 0;
        
        prepare_message(&msg, input);
        
        if (send(sock, &msg, sizeof(Message), 0) < 0) {
            perror("send");
            break;
        }
        
        if (msg.type == MSG_BYE) {
            std::cout << "Disconnected" << std::endl;
            break;
        }
        
        if (msg.type == MSG_PING) {
            if (!handle_server_response(sock)) {
                break;
            }
        }
    }
}

int main() {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    int sock;
    struct sockaddr_in server_addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return 1;
    }
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }
    
    std::cout << "Connected to " << SERVER_IP << ":" << SERVER_PORT << std::endl;
    
    if (!handshake_with_server(sock)) {
        close(sock);
        return 1;
    }
    
    communication_loop(sock);
    
    close(sock);
    std::cout << "Client shutdown" << std::endl;
    return 0;
}