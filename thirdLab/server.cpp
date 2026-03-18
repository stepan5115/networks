#include <iostream>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <csignal>
#include <vector>
#include <queue>
#include <algorithm>
#include "structs.h"

#define PORT 8080
#define THREAD_POOL_SIZE 10
#define MAX_QUEUE_SIZE 100
#define MAX_CLIENTS 100

bool keepRunning = true;

std::vector<ClientInfo> clients;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

std::queue<int> client_queue;

void handleSignal(int /*signal*/) {
    keepRunning = false;
    pthread_cond_broadcast(&queue_cond);
}

Message ntoh_message(const Message& net_msg) {
    Message host_msg = net_msg;
    host_msg.length = ntohl(net_msg.length);
    return host_msg;
}

Message hton_message(const Message& host_msg) {
    Message net_msg = host_msg;
    net_msg.length = htonl(host_msg.length);
    return net_msg;
}

bool send_message(int socket, const Message& msg) {
    Message net_msg = hton_message(msg);
    if (send(socket, &net_msg, sizeof(Message), 0) < 0) {
        return false;
    }
    return true;
}

bool recv_message(int socket, Message& msg) {
    if (recv(socket, &msg, sizeof(Message), 0) <= 0) {
        return false;
    }
    msg = ntoh_message(msg);
    return true;
}

ClientInfo* find_client(int socket) {
    for (auto& client : clients) {
        if (client.socket == socket) {
            return &client;
        }
    }
    return nullptr;
}

void remove_client(int socket) {
    pthread_mutex_lock(&clients_mutex);
    
    auto it = std::find_if(clients.begin(), clients.end(), 
        [socket](const ClientInfo& c) { return c.socket == socket; });
    
    if (it != clients.end()) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &it->address.sin_addr, ip, INET_ADDRSTRLEN);
        std::cout << "Client disconnected: " << it->nickname << " [" 
                  << ip << ":" << ntohs(it->address.sin_port) << "]" << std::endl;
        
        close(it->socket);
        clients.erase(it);
    }
    
    pthread_mutex_unlock(&clients_mutex);
}

void broadcast_message(const Message& msg, int sender_socket) {
    pthread_mutex_lock(&clients_mutex);
    
    ClientInfo* sender = find_client(sender_socket);
    if (!sender) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }
    
    char full_message[MAX_PAYLOAD + MAX_PAYLOAD + 4];
    snprintf(full_message, sizeof(full_message), "[%s]: %s", 
             sender->nickname, msg.payload);
    
    Message broadcast_msg;
    memset(&broadcast_msg, 0, sizeof(Message));
    broadcast_msg.type = MSG_TEXT;
    strncpy(broadcast_msg.payload, full_message, MAX_PAYLOAD - 1);
    broadcast_msg.length = sizeof(broadcast_msg.type) + strlen(broadcast_msg.payload) + 1;
    
    for (const auto& client : clients) {
        if (client.socket != sender_socket && client.active) {
            if (!send_message(client.socket, broadcast_msg)) {
                std::cout << "Failed to send to client " << client.nickname << std::endl;
            }
        }
    }
    
    pthread_mutex_unlock(&clients_mutex);
}

void* handle_client(int client_sock) {
    
    Message msg;
    char client_ip[INET_ADDRSTRLEN];
    ClientInfo client;
    client.socket = client_sock;
    client.active = true;
    
    socklen_t addr_len = sizeof(client.address);
    getpeername(client_sock, (struct sockaddr*)&client.address, &addr_len);
    inet_ntop(AF_INET, &client.address.sin_addr, client_ip, INET_ADDRSTRLEN);
    
    if (!recv_message(client_sock, msg) || msg.type != MSG_HELLO) {
        std::cout << "Failed to receive HELLO from client" << std::endl;
        close(client_sock);
        return nullptr;
    }
    
    strncpy(client.nickname, msg.payload, MAX_PAYLOAD - 1);
    std::cout << "Client connected: " << client.nickname << " [" 
              << client_ip << ":" << ntohs(client.address.sin_port) << "]" << std::endl;
    
    msg.type = MSG_WELCOME;
    snprintf(msg.payload, MAX_PAYLOAD, "Welcome %s! Total users: %zu", 
             client.nickname, clients.size() + 1);
    msg.length = sizeof(msg.type) + strlen(msg.payload) + 1;
    
    if (!send_message(client_sock, msg)) {
        close(client_sock);
        return nullptr;
    }
    
    pthread_mutex_lock(&clients_mutex);
    clients.push_back(client);
    pthread_mutex_unlock(&clients_mutex);
    
    while (keepRunning && client.active) {
        if (!recv_message(client_sock, msg)) {
            std::cout << "Client " << client.nickname << " disconnected" << std::endl;
            break;
        }
        
        switch (msg.type) {
            case MSG_TEXT:
                std::cout << "[" << client.nickname << "]: " << msg.payload << std::endl;
                broadcast_message(msg, client_sock);
                break;
                
            case MSG_PING:
                std::cout << "Received PING from " << client.nickname << ", sending PONG" << std::endl;
                msg.type = MSG_PONG;
                strcpy(msg.payload, "PONG");
                msg.length = sizeof(msg.type) + strlen(msg.payload) + 1;
                send_message(client_sock, msg);
                break;
                
            case MSG_BYE:
                std::cout << "Client " << client.nickname << " requested to disconnect" << std::endl;
                client.active = false;
                break;
                
            default:
                std::cout << "Unknown message type from " << client.nickname << ": " 
                          << msg.type << std::endl;
        }
    }
    
    remove_client(client_sock);
    return nullptr;
}

void* worker_thread(void* /*arg*/) {
    while (keepRunning) {
        pthread_mutex_lock(&queue_mutex);
        
        while (client_queue.empty() && keepRunning) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        
        if (!keepRunning || client_queue.empty()) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        
        int client_sock = client_queue.front();
        client_queue.pop();
        
        pthread_mutex_unlock(&queue_mutex);
        
        handle_client(client_sock);
    }
    return nullptr;
}

void add_to_queue(int client_sock) {
    pthread_mutex_lock(&queue_mutex);
    
    if (client_queue.size() < MAX_QUEUE_SIZE) {
        client_queue.push(client_sock);
        pthread_cond_signal(&queue_cond);
    } else {
        std::cout << "Queue is full, rejecting client" << std::endl;
        close(client_sock);
    }
    
    pthread_mutex_unlock(&queue_mutex);
}

int main() {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    int server_sock;
    struct sockaddr_in server_addr;
    
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        exit(1);
    }
    
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_sock);
        exit(1);
    }
    
    if (listen(server_sock, 10) < 0) {
        perror("listen");
        close(server_sock);
        exit(1);
    }
    
    std::cout << "Server listening on port " << PORT << std::endl;
    
    pthread_t thread_pool[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&thread_pool[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create");
            exit(1);
        }
        pthread_detach(thread_pool[i]);
    }
    
    while (keepRunning) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            if (keepRunning) {
                perror("accept");
            }
            continue;
        }
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "New connection from " << client_ip << ":" 
                  << ntohs(client_addr.sin_port) << std::endl;
        
        add_to_queue(client_sock);
    }
    
    close(server_sock);
    
    pthread_mutex_lock(&clients_mutex);
    for (auto& client : clients) {
        close(client.socket);
    }
    clients.clear();
    pthread_mutex_unlock(&clients_mutex);
    
    std::cout << "Server shutting down" << std::endl;
    return 0;
}