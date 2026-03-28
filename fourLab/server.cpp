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

void log_layer(int layer, const std::string& message) {
    std::cout << "[Layer " << layer << " - ";
    switch(layer) {
        case 4: std::cout << "Transport"; break;
        case 5: std::cout << "Session"; break;
        case 6: std::cout << "Presentation"; break;
        case 7: std::cout << "Application"; break;
        default: std::cout << "Unknown";
    }
    std::cout << "] " << message << std::endl;
}

Message ntoh_message(const Message& net_msg) {
    log_layer(6, "deserialize Message (network to host)");
    Message host_msg = net_msg;
    host_msg.length = ntohl(net_msg.length);
    return host_msg;
}

Message hton_message(const Message& host_msg) {
    log_layer(6, "serialize Message (host to network)");
    Message net_msg = host_msg;
    net_msg.length = htonl(host_msg.length);
    return net_msg;
}

bool send_message(int socket, const Message& msg) {
    log_layer(4, "send() - transmitting data");
    Message net_msg = hton_message(msg);
    if (send(socket, &net_msg, sizeof(Message), 0) < 0) {
        return false;
    }
    return true;
}

bool recv_message(int socket, Message& msg) {
    log_layer(4, "recv() - receiving data");
    if (recv(socket, &msg, sizeof(Message), 0) <= 0) {
        return false;
    }
    msg = ntoh_message(msg);
    return true;
}

ClientInfo* find_client_by_socket(int socket) {
    for (auto& client : clients) {
        if (client.socket == socket) {
            return &client;
        }
    }
    return nullptr;
}

ClientInfo* find_client_by_nickname(const std::string& nickname) {
    for (auto& client : clients) {
        if (client.authenticated && std::string(client.nickname) == nickname) {
            return &client;
        }
    }
    return nullptr;
}

bool is_nickname_unique(const std::string& nickname) {
    for (const auto& client : clients) {
        if (client.authenticated && std::string(client.nickname) == nickname) {
            return false;
        }
    }
    return true;
}

void remove_client(int socket) {
    pthread_mutex_lock(&clients_mutex);
    
    auto it = std::find_if(clients.begin(), clients.end(), 
        [socket](const ClientInfo& c) { return c.socket == socket; });
    
    if (it != clients.end()) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &it->address.sin_addr, ip, INET_ADDRSTRLEN);
        
        if (it->authenticated) {
            Message info_msg;
            memset(&info_msg, 0, sizeof(Message));
            info_msg.type = MSG_SERVER_INFO;
            snprintf(info_msg.payload, MAX_PAYLOAD, "User [%s] disconnected", it->nickname);
            info_msg.length = sizeof(info_msg.type) + strlen(info_msg.payload) + 1;
            
            for (const auto& client : clients) {
                if (client.socket != socket && client.authenticated) {
                    send_message(client.socket, info_msg);
                }
            }
            
            std::cout << "User [" << it->nickname << "] disconnected" << std::endl;
        }
        
        close(it->socket);
        clients.erase(it);
    }
    
    pthread_mutex_unlock(&clients_mutex);
}

void broadcast_message(const Message& msg, int sender_socket) {
    log_layer(7, "broadcast message");
    
    pthread_mutex_lock(&clients_mutex);
    
    ClientInfo* sender = find_client_by_socket(sender_socket);
    if (!sender || !sender->authenticated) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }
    
    for (const auto& client : clients) {
        if (client.socket != sender_socket && client.authenticated) {
            if (!send_message(client.socket, msg)) {
                std::cout << "Failed to send to client " << client.nickname << std::endl;
            }
        }
    }
    
    pthread_mutex_unlock(&clients_mutex);
}

void send_private_message(const std::string& target_nick, const std::string& message, int sender_socket) {
    log_layer(7, "handle private message");
    
    pthread_mutex_lock(&clients_mutex);
    
    ClientInfo* sender = find_client_by_socket(sender_socket);
    if (!sender || !sender->authenticated) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }
    
    ClientInfo* target = find_client_by_nickname(target_nick);
    if (!target) {
        Message error_msg;
        memset(&error_msg, 0, sizeof(Message));
        error_msg.type = MSG_ERROR;
        snprintf(error_msg.payload, MAX_PAYLOAD, "User '%s' not found", target_nick.c_str());
        error_msg.length = sizeof(error_msg.type) + strlen(error_msg.payload) + 1;
        send_message(sender_socket, error_msg);
        pthread_mutex_unlock(&clients_mutex);
        return;
    }
    
    Message private_msg;
    memset(&private_msg, 0, sizeof(Message));
    private_msg.type = MSG_PRIVATE;
    snprintf(private_msg.payload, MAX_PAYLOAD, "[PRIVATE][%s]: %s", 
             sender->nickname, message.c_str());
    private_msg.length = sizeof(private_msg.type) + strlen(private_msg.payload) + 1;
    
    if (!send_message(target->socket, private_msg)) {
        Message error_msg;
        memset(&error_msg, 0, sizeof(Message));
        error_msg.type = MSG_ERROR;
        snprintf(error_msg.payload, MAX_PAYLOAD, "Failed to send message to '%s'", target_nick.c_str());
        error_msg.length = sizeof(error_msg.type) + strlen(error_msg.payload) + 1;
        send_message(sender_socket, error_msg);
    } else {
        std::cout << "[PRIVATE] " << sender->nickname << " -> " << target_nick << ": " << message << std::endl;
    }
    
    pthread_mutex_unlock(&clients_mutex);
}

bool authenticate_client(int client_sock, const std::string& nickname) {
    log_layer(5, "authentication process started");
    
    if (nickname.empty()) {
        log_layer(5, "authentication failed: empty nickname");
        Message error_msg;
        memset(&error_msg, 0, sizeof(Message));
        error_msg.type = MSG_ERROR;
        strcpy(error_msg.payload, "Nickname cannot be empty");
        error_msg.length = sizeof(error_msg.type) + strlen(error_msg.payload) + 1;
        send_message(client_sock, error_msg);
        return false;
    }
    
    if (!is_nickname_unique(nickname)) {
        log_layer(5, "authentication failed: nickname not unique");
        Message error_msg;
        memset(&error_msg, 0, sizeof(Message));
        error_msg.type = MSG_ERROR;
        snprintf(error_msg.payload, MAX_PAYLOAD, "Nickname '%s' is already taken", nickname.c_str());
        error_msg.length = sizeof(error_msg.type) + strlen(error_msg.payload) + 1;
        send_message(client_sock, error_msg);
        return false;
    }
    
    // Находим клиента в списке по сокету
    ClientInfo* client = find_client_by_socket(client_sock);
    if (!client) {
        log_layer(5, "authentication failed: client not found");
        return false;
    }
    
    strncpy(client->nickname, nickname.c_str(), MAX_NICKNAME - 1);
    client->authenticated = true;
    
    log_layer(5, "authentication success: " + nickname);
    
    // Отправляем приветственное сообщение
    Message welcome_msg;
    memset(&welcome_msg, 0, sizeof(Message));
    welcome_msg.type = MSG_WELCOME;
    snprintf(welcome_msg.payload, MAX_PAYLOAD, "Welcome %s! Total users: %zu", 
             nickname.c_str(), clients.size());
    welcome_msg.length = sizeof(welcome_msg.type) + strlen(welcome_msg.payload) + 1;
    send_message(client_sock, welcome_msg);
    
    // Отправляем системное сообщение о подключении
    Message info_msg;
    memset(&info_msg, 0, sizeof(Message));
    info_msg.type = MSG_SERVER_INFO;
    snprintf(info_msg.payload, MAX_PAYLOAD, "User [%s] connected", nickname.c_str());
    info_msg.length = sizeof(info_msg.type) + strlen(info_msg.payload) + 1;
    
    for (const auto& c : clients) {
        if (c.socket != client_sock && c.authenticated) {
            send_message(c.socket, info_msg);
        }
    }
    
    std::cout << "User [" << nickname << "] connected" << std::endl;
    return true;
}

void* handle_client(int client_sock) {
    log_layer(7, "new client connection handler started");
    
    Message msg;
    char client_ip[INET_ADDRSTRLEN];
    ClientInfo client;
    client.socket = client_sock;
    client.authenticated = false;
    client.active = true;
    
    socklen_t addr_len = sizeof(client.address);
    getpeername(client_sock, (struct sockaddr*)&client.address, &addr_len);
    inet_ntop(AF_INET, &client.address.sin_addr, client_ip, INET_ADDRSTRLEN);
    
    pthread_mutex_lock(&clients_mutex);
    clients.push_back(client);
    pthread_mutex_unlock(&clients_mutex);
    
    if (!recv_message(client_sock, msg) || msg.type != MSG_AUTH) {
        log_layer(7, "authentication required, closing connection");
        Message error_msg;
        memset(&error_msg, 0, sizeof(Message));
        error_msg.type = MSG_ERROR;
        strcpy(error_msg.payload, "Authentication required");
        error_msg.length = sizeof(error_msg.type) + strlen(error_msg.payload) + 1;
        send_message(client_sock, error_msg);
        remove_client(client_sock);
        return nullptr;
    }
    
    log_layer(7, "received MSG_AUTH, payload: " + std::string(msg.payload));
    
    if (!authenticate_client(client_sock, msg.payload)) {
        log_layer(7, "authentication failed, closing connection");
        remove_client(client_sock);
        return nullptr;
    }
    
    while (keepRunning && client.active) {
        if (!recv_message(client_sock, msg)) {
            log_layer(7, "client disconnected");
            break;
        }
        
        log_layer(7, "processing message type: " + std::to_string(msg.type));
        
        switch (msg.type) {
            case MSG_TEXT:
                log_layer(7, "broadcasting text message: " + std::string(msg.payload));
                std::cout << "[" << find_client_by_socket(client_sock)->nickname 
                          << "]: " << msg.payload << std::endl;
                
                Message broadcast_msg;
                memset(&broadcast_msg, 0, sizeof(Message));
                broadcast_msg.type = MSG_TEXT;
                snprintf(broadcast_msg.payload, MAX_PAYLOAD, "[%s]: %s", 
                         find_client_by_socket(client_sock)->nickname, msg.payload);
                broadcast_msg.length = sizeof(broadcast_msg.type) + strlen(broadcast_msg.payload) + 1;
                broadcast_message(broadcast_msg, client_sock);
                break;
                
            case MSG_PRIVATE:
                log_layer(7, "processing private message");
                {
                    std::string payload(msg.payload);
                    size_t colon_pos = payload.find(':');
                    if (colon_pos != std::string::npos) {
                        std::string target = payload.substr(0, colon_pos);
                        std::string message = payload.substr(colon_pos + 1);
                        send_private_message(target, message, client_sock);
                    } else {
                        Message error_msg;
                        memset(&error_msg, 0, sizeof(Message));
                        error_msg.type = MSG_ERROR;
                        strcpy(error_msg.payload, "Invalid private message format");
                        error_msg.length = sizeof(error_msg.type) + strlen(error_msg.payload) + 1;
                        send_message(client_sock, error_msg);
                    }
                }
                break;
                
            case MSG_PING:
                log_layer(7, "responding to PING with PONG");
                msg.type = MSG_PONG;
                strcpy(msg.payload, "PONG");
                msg.length = sizeof(msg.type) + strlen(msg.payload) + 1;
                send_message(client_sock, msg);
                break;
                
            case MSG_BYE:
                log_layer(7, "client requested disconnect");
                client.active = false;
                break;
                
            default:
                log_layer(7, "unknown message type: " + std::to_string(msg.type));
                std::cout << "Unknown message type from " 
                          << find_client_by_socket(client_sock)->nickname 
                          << ": " << msg.type << std::endl;
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
    std::cout << "OSI Layer visualization enabled" << std::endl;
    std::cout << "==================================" << std::endl;
    
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