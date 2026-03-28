#include <iostream>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <csignal>
#include <string>
#include "structs.h"

#define SERVER_PORT 8080
#define SERVER_IP "127.0.0.1"
#define RECONNECT_DELAY 2
#define INPUT_TIMEOUT 1

bool keepRunning = true;
bool connected = false;
int sock = -1;
pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;

bool read_input_with_timeout(char* buffer, int max_len, int timeout_seconds) {
    fd_set readfds;
    struct timeval tv;
    
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    
    tv.tv_sec = timeout_seconds;
    tv.tv_usec = 0;
    
    int result = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
    
    if (result > 0) {
        if (fgets(buffer, max_len, stdin) != NULL) {
            buffer[strcspn(buffer, "\n")] = 0;
            return true;
        }
    }
    return false;
}

void handleSignal(int /*signal*/) {
    keepRunning = false;
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

void* receive_thread(void* /*arg*/) {
    Message msg;
    
    while (keepRunning && connected) {
        pthread_mutex_lock(&sock_mutex);
        int current_sock = sock;
        pthread_mutex_unlock(&sock_mutex);
        
        if (current_sock < 0) {
            break;
        }

        fd_set readfds;
        struct timeval tv;
        
        FD_ZERO(&readfds);
        FD_SET(current_sock, &readfds);

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int result = select(current_sock + 1, &readfds, NULL, NULL, &tv);

        if (result == 0) {
            continue;
        }

        if (result < 0) {
            break;
        }
        
        if (!recv_message(current_sock, msg)) {
            std::cout << "\nConnection to server lost" << std::endl;
            pthread_mutex_lock(&sock_mutex);
            connected = false;
            close(sock);
            sock = -1;
            pthread_mutex_unlock(&sock_mutex);
            break;
        }
        
        switch (msg.type) {
            case MSG_WELCOME:
                std::cout << "\n*** " << msg.payload << " ***" << std::endl;
                break;
                
            case MSG_TEXT:
                std::cout << "\n" << msg.payload << std::endl;
                break;
                
            case MSG_PRIVATE:
                std::cout << "\n" << msg.payload << std::endl;
                break;
                
            case MSG_SERVER_INFO:
                std::cout << "\n[SERVER]: " << msg.payload << std::endl;
                break;
                
            case MSG_ERROR:
                std::cout << "\n[ERROR]: " << msg.payload << std::endl;
                break;
                
            case MSG_PONG:
                std::cout << "\n*** PONG received ***" << std::endl;
                break;
                
            case MSG_BYE:
                std::cout << "\n*** Server closed connection ***" << std::endl;
                pthread_mutex_lock(&sock_mutex);
                connected = false;
                close(sock);
                sock = -1;
                pthread_mutex_unlock(&sock_mutex);
                break;
                
            default:
                std::cout << "\n*** Unknown message type: " << msg.type << " ***" << std::endl;
        }
        
        std::cout << "> ";
        fflush(stdout);
    }
    
    return nullptr;
}

int connect_to_server() {
    int new_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (new_sock < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(new_sock);
        return -1;
    }
    
    if (connect(new_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(new_sock);
        return -1;
    }
    
    return new_sock;
}

bool authenticate(int socket, const std::string& nickname) {
    Message auth_msg;
    memset(&auth_msg, 0, sizeof(Message));
    auth_msg.type = MSG_AUTH;
    strncpy(auth_msg.payload, nickname.c_str(), MAX_PAYLOAD - 1);
    auth_msg.length = sizeof(auth_msg.type) + strlen(auth_msg.payload) + 1;
    
    if (!send_message(socket, auth_msg)) {
        return false;
    }
    
    Message response;
    if (!recv_message(socket, response)) {
        return false;
    }
    
    if (response.type == MSG_ERROR) {
        std::cout << "Authentication failed: " << response.payload << std::endl;
        return false;
    }
    
    if (response.type == MSG_WELCOME) {
        std::cout << response.payload << std::endl;
        return true;
    }
    
    return false;
}

int main() {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);
    
    pthread_t recv_thread;
    
    std::cout << "=== Chat Client ===" << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  /w <nick> <message> - Send private message" << std::endl;
    std::cout << "  /ping                - Ping the server" << std::endl;
    std::cout << "  /quit                - Disconnect" << std::endl;
    std::cout << std::endl;
    
    std::string nickname;
    std::cout << "Enter your nickname: ";
    std::getline(std::cin, nickname);
    
    while (nickname.empty()) {
        std::cout << "Nickname cannot be empty. Enter your nickname: ";
        std::getline(std::cin, nickname);
    }
    
    while (keepRunning) {
        std::cout << "Connecting to " << SERVER_IP << ":" << SERVER_PORT << "..." << std::endl;
        
        pthread_mutex_lock(&sock_mutex);
        sock = connect_to_server();
        pthread_mutex_unlock(&sock_mutex);
        
        if (sock < 0) {
            std::cout << "Connection failed. Retrying in " << RECONNECT_DELAY << " seconds..." << std::endl;
            sleep(RECONNECT_DELAY);
            continue;
        }
        
        if (!authenticate(sock, nickname)) {
            std::cout << "Authentication failed. Retrying..." << std::endl;
            pthread_mutex_lock(&sock_mutex);
            close(sock);
            sock = -1;
            pthread_mutex_unlock(&sock_mutex);
            sleep(RECONNECT_DELAY);
            continue;
        }
        
        connected = true;
        std::cout << "Connected to server. Type messages:" << std::endl;
        
        if (pthread_create(&recv_thread, NULL, receive_thread, NULL) != 0) {
            perror("pthread_create");
            break;
        }
        
        char input[MAX_PAYLOAD];
        std::cout << "> " << std::flush;
        while (keepRunning && connected) {
            if (read_input_with_timeout(input, MAX_PAYLOAD, INPUT_TIMEOUT)) {
                if (strlen(input) == 0) {
                    continue;
                }
            
                Message msg;
                memset(&msg, 0, sizeof(Message));
            
                if (strcmp(input, "/quit") == 0) {
                    msg.type = MSG_BYE;
                    strcpy(msg.payload, "bye");
                    msg.length = sizeof(msg.type) + strlen(msg.payload) + 1;
                    
                    pthread_mutex_lock(&sock_mutex);
                    if (sock >= 0) {
                        send_message(sock, msg);
                    }
                    pthread_mutex_unlock(&sock_mutex);
                    
                    connected = false;
                    keepRunning = false;
                    break;
                }
                else if (strcmp(input, "/ping") == 0) {
                    msg.type = MSG_PING;
                    strcpy(msg.payload, "ping");
                    msg.length = sizeof(msg.type) + strlen(msg.payload) + 1;
                    
                    pthread_mutex_lock(&sock_mutex);
                    if (sock >= 0) {
                        send_message(sock, msg);
                    }
                    pthread_mutex_unlock(&sock_mutex);
                }
                else if (strncmp(input, "/w ", 3) == 0) {
                    char* target = strtok(input + 3, " ");
                    char* message = strtok(NULL, "");
                    
                    if (target && message && strlen(message) > 0) {
                        msg.type = MSG_PRIVATE;
                        snprintf(msg.payload, MAX_PAYLOAD, "%s:%s", target, message);
                        msg.length = sizeof(msg.type) + strlen(msg.payload) + 1;
                        
                        pthread_mutex_lock(&sock_mutex);
                        if (sock >= 0) {
                            send_message(sock, msg);
                        }
                        pthread_mutex_unlock(&sock_mutex);
                    } else {
                        std::cout << "Usage: /w <nickname> <message>" << std::endl;
                    }
                }
                else {
                    msg.type = MSG_TEXT;
                    strncpy(msg.payload, input, MAX_PAYLOAD - 1);
                    msg.length = sizeof(msg.type) + strlen(msg.payload) + 1;
                    
                    pthread_mutex_lock(&sock_mutex);
                    if (sock >= 0) {
                        send_message(sock, msg);
                    }
                    pthread_mutex_unlock(&sock_mutex);
                }
                std::cout << "> " << std::flush;
            }
        }
        
        pthread_join(recv_thread, NULL);
        
        if (keepRunning && !connected) {
            std::cout << "Attempting to reconnect in " << RECONNECT_DELAY << " seconds..." << std::endl;
            sleep(RECONNECT_DELAY);
        }
    }
    
    pthread_mutex_lock(&sock_mutex);
    if (sock >= 0) {
        close(sock);
    }
    pthread_mutex_unlock(&sock_mutex);
    
    std::cout << "Client shutdown" << std::endl;
    return 0;
}