#include <cstdint>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <csignal>
#include <string>
#include<deque>
#include <iomanip>
#include "structs.h"

#define SERVER_PORT 8080
#define SERVER_IP "127.0.0.1"
#define RECONNECT_DELAY 2
#define INPUT_TIMEOUT 1

bool keepRunning = true;
bool connected = false;
int sock = -1;
uint32_t current_id = 0;
std::deque<PendingMsg> pendings;
std::deque<PingRecord> ping_records;
PingResults last_ping_results;
pthread_mutex_t results_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t redraw_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t ping_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t pendings_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;


void print_service_message(const std::string& msg) {
    pthread_mutex_lock(&redraw_mutex);
    std::cout << "\r\033[K";
    std::cout << msg << std::endl;
    std::cout << "> " << std::flush;
    pthread_mutex_unlock(&redraw_mutex);
}

void print_progress_bar(int current, int total, int width = 50) {
    if (total <= 0) return;
    
    float progress = static_cast<float>(current) / total;
    int filled = static_cast<int>(progress * width);
    
    std::string bar = "[" + std::string(filled, '=') + 
                      std::string(width - filled, ' ') + "]";
    
    int percent = static_cast<int>(progress * 100);
    
    std::cout << "\r" << bar << " " << std::setw(3) << percent << "%";
    std::cout << " (" << current << "/" << total << ")" << std::flush;
    
    if (current == total) {
        std::cout << std::endl;
    }
}

std::string format_timestamp(time_t timestamp) {
    struct tm* tm_info = localtime(&timestamp);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buffer);
}

void handleSignal(int /*signal*/) {
    keepRunning = false;
}

void save_netdiag_results(const std::string& nickname, const PingResults& results) {
    std::string filename = "net_diag_" + nickname + ".json";
    
    std::string json_content = "{\n";
    json_content += "  \"nickname\": \"" + nickname + "\",\n";
    json_content += "  \"timestamp\": \"" + format_timestamp(results.timestamp) + "\",\n";
    json_content += "  \"total_sent\": " + std::to_string(results.total_sent) + ",\n";
    json_content += "  \"received\": " + std::to_string(results.received) + ",\n";
    json_content += "  \"lost\": " + std::to_string(results.total_sent - results.received) + ",\n";
    json_content += "  \"loss_percent\": " + std::to_string(results.loss_percent) + ",\n";
    json_content += "  \"rtt_avg_ms\": " + std::to_string(results.avg_rtt_ms) + ",\n";
    json_content += "  \"jitter_avg_ms\": " + std::to_string(results.avg_jitter_ms) + ",\n";
    json_content += "  \"metrics\": {\n";
    json_content += "    \"rtt_avg\": " + std::to_string(results.avg_rtt_ms / 1000.0) + ",\n";
    json_content += "    \"jitter_avg\": " + std::to_string(results.avg_jitter_ms / 1000.0) + ",\n";
    json_content += "    \"loss\": " + std::to_string(results.loss_percent) + "\n";
    json_content += "  }\n";
    json_content += "}\n";
    
    FILE* file = fopen(filename.c_str(), "w");
    if (file) {
        fprintf(file, "%s", json_content.c_str());
        fclose(file);
        print_service_message("[NETDIAG] Results saved to " + filename);
    } else {
        print_service_message("[NETDIAG] Failed to save results to " + filename);
    }
}

MessageEx ntoh_message(const MessageEx& net_msg) {
    MessageEx host_msg = net_msg;
    host_msg.length = ntohl(net_msg.length);
    host_msg.msg_id = ntohl(net_msg.msg_id);
    return host_msg;
}

MessageEx hton_message(const MessageEx& host_msg) {
    MessageEx net_msg = host_msg;
    net_msg.length = htonl(host_msg.length);
    net_msg.msg_id = htonl(host_msg.msg_id);
    return net_msg;
}

bool send_message(int socket, const MessageEx& msg) {
    MessageEx net_msg = hton_message(msg);
    if (send(socket, &net_msg, sizeof(MessageEx), 0) < 0) {
        return false;
    }
    return true;
}

bool recv_message(int socket, MessageEx& msg) {
    if (recv(socket, &msg, sizeof(MessageEx), 0) <= 0) {
        return false;
    }
    msg = ntoh_message(msg);
    return true;
}

void* receive_thread(void* /*arg*/) {
    MessageEx msg;
    
    while (keepRunning && connected) {
        pthread_mutex_lock(&sock_mutex);
        int current_sock = sock;
        pthread_mutex_unlock(&sock_mutex);
        
        if (current_sock < 0) {
            break;
        }
        
        if (!recv_message(current_sock, msg)) {
            print_service_message("Connection to server lost");
            pthread_mutex_lock(&sock_mutex);
            connected = false;
            close(sock);
            sock = -1;
            pthread_mutex_unlock(&sock_mutex);
            break;
        }

        std::string output;
        
        switch (msg.type) {
            case MSG_ACK: {
                uint32_t ack_id = msg.msg_id;
                pthread_mutex_lock(&pendings_mutex);
                auto it = pendings.begin();
                while (it != pendings.end()) {
                    if (it->msg.msg_id == ack_id) {
                        output = "[Transport][RETRY] Message " + std::to_string(ack_id) + 
                                " acknowledged (attempts: " + std::to_string(it->retries) + ")";
                        print_service_message(output);
                        it = pendings.erase(it);
                        break;
                    } else {
                        ++it;
                    }
                }
                pthread_mutex_unlock(&pendings_mutex);
                break;
            }
            case MSG_WELCOME:
                print_service_message("*** " + std::string(msg.payload) + " ***");
                break;
                
            case MSG_TEXT:
                output = "[" + format_timestamp(msg.timestamp) + "]" +
                        "[id=" + std::to_string(msg.msg_id) + "]" +
                        "[" + msg.sender + "]: " + msg.payload;
                print_service_message(output);
                break;
                
            case MSG_PRIVATE:
                output = "[PRIVATE][" +
                        format_timestamp(msg.timestamp) + "]" +
                        "[id=" + std::to_string(msg.msg_id) + "]" +
                        "[" + msg.sender + "->" + msg.receiver + "]: " +
                        msg.payload;
                print_service_message(output);
                break;
                
            case MSG_SERVER_INFO:
                print_service_message("[SERVER]: " + std::string(msg.payload));
                break;
                
            case MSG_ERROR:
                print_service_message("[ERROR]: " + std::string(msg.payload));
                break;
                
            case MSG_PONG: {
                uint32_t pong_id = msg.msg_id;
                time_t now = time(nullptr);
                pthread_mutex_lock(&ping_mutex);
                auto it = ping_records.begin();
                while (it != ping_records.end()) {
                    if (it->msg_id == pong_id && !it->received) {
                        it->received = true;
                        it->rtt = difftime(now, it->send_time);
                        //print_service_message("[PONG] Received response for ID " + std::to_string(pong_id));
                        break;
                    }
                    ++it;
                }
                pthread_mutex_unlock(&ping_mutex);
                break;
            }

            case MSG_BYE:
                print_service_message("*** Server closed connection ***");
                pthread_mutex_lock(&sock_mutex);
                connected = false;
                close(sock);
                sock = -1;
                pthread_mutex_unlock(&sock_mutex);
                break;
            
            case MSG_HISTORY_DATA:
                output = "[HISTORY]:\n" + std::string(msg.payload);
                print_service_message(output);
                break;
                
            default:
                print_service_message("*** Unknown message type: " + std::to_string(msg.type) + " ***");
        }
    }
    
    return nullptr;
}

void* sending_thread(void* /*arg*/) {
    while (keepRunning && connected) {
        pthread_mutex_lock(&sock_mutex);
        int current_sock = sock;
        pthread_mutex_unlock(&sock_mutex);
        if (current_sock < 0) {
            break;
        }
        pthread_mutex_lock(&pendings_mutex);
        if (!pendings.empty()) {
            PendingMsg& pending = pendings.front();
            time_t now = time(nullptr);
            bool should_send = false;
            if (pending.retries == 0) {
                should_send = true;
                pending.msg.timestamp = time(nullptr);
                if (pending.msg.type == MSG_PING) {
                    PingRecord req;
                    req.msg_id = pending.msg.msg_id;
                    req.send_time = pending.msg.timestamp;
                    req.received = false;
                    req.rtt = 0;

                    pthread_mutex_lock(&ping_mutex);
                    ping_records.push_back(req);
                    pthread_mutex_unlock(&ping_mutex);
                }
            } else if (pending.retries < MAX_ATTEMPS_RESEND) {
                if (now - pending.last_send_time >= MAX_TIMEOUT_SECONDS) {
                    should_send = true;
                }
            } else if (now - pending.last_send_time >= MAX_TIMEOUT_SECONDS) {
                std::string msg = "[Transport][RETRY] Message " + 
                                 std::to_string(pending.msg.msg_id) + 
                                 " lost after " + std::to_string(MAX_ATTEMPS_RESEND) + " attempts";
                print_service_message(msg);
                pendings.pop_front();
                pthread_mutex_unlock(&pendings_mutex);
                usleep(100000);
                continue;
            }
            if (should_send) {
                pending.retries++;
                pending.last_send_time = now;
                std::string msg = "[Transport][RETRY] " + 
                                 std::string(pending.retries == 1 ? "Sending" : "Resending") +
                                 " message " + std::to_string(pending.msg.msg_id) + 
                                 " attempt " + std::to_string(pending.retries);
                print_service_message(msg);
                if (send_message(current_sock, pending.msg)) {
                    pthread_mutex_unlock(&pendings_mutex);
                    usleep(10000);
                } else {
                    print_service_message("[Transport] Send failed, will retry");
                    pthread_mutex_unlock(&pendings_mutex);
                    usleep(100000);
                }
            } else {
                pthread_mutex_unlock(&pendings_mutex);
                usleep(100000);
            }
        } else {
            pthread_mutex_unlock(&pendings_mutex);
            usleep(100000);
        }
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
    MessageEx auth_msg;
    memset(&auth_msg, 0, sizeof(MessageEx));
    strncpy(auth_msg.payload, nickname.c_str(), MAX_NAME - 1);
    auth_msg.payload[MAX_NAME-1] = '\0';
    auth_msg.length = strlen(auth_msg.payload) + 1;
    auth_msg.type = MSG_AUTH;
    
    if (!send_message(socket, auth_msg)) {
        return false;
    }
    
    MessageEx response;
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
    pthread_t send_thread;
    
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
            std::cout << "Authentication failed" << std::endl;
            pthread_mutex_lock(&sock_mutex);
            close(sock);
            sock = -1;
            pthread_mutex_unlock(&sock_mutex);
            sleep(RECONNECT_DELAY);
            keepRunning = false;
            break;
        }
        
        connected = true;
        std::cout << "Connected to server. Type messages:" << std::endl;
        
        if (pthread_create(&recv_thread, NULL, receive_thread, NULL) != 0) {
            perror("pthread_create");
            break;
        }

        if (pthread_create(&send_thread, NULL, sending_thread, NULL) != 0) {
            perror("pthread_create");
            break;
        }
        char input[MAX_PAYLOAD];
        while (keepRunning && connected) {
            std::cout << "> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) {
                break;
            }
            if (!keepRunning) {
                break;
            }
            if (line.empty()) {
                continue;
            }
            strncpy(input, line.c_str(), MAX_PAYLOAD - 1);
            input[MAX_PAYLOAD - 1] = '\0';

            MessageEx msg;
            PendingMsg pending;
            memset(&msg, 0, sizeof(MessageEx));
        
            if (strcmp(input, "/quit") == 0) {
                strcpy(msg.payload, "bye");
                msg.length = strlen(msg.payload) + 1;
                msg.type = MSG_BYE;
                msg.msg_id = current_id++;
                
                pthread_mutex_lock(&sock_mutex);
                if (sock >= 0) {
                    send_message(sock, msg);
                }
                pthread_mutex_unlock(&sock_mutex);
                
                connected = false;
                keepRunning = false;
                break;
            }
            else if (strncmp(input, "/ping", 5) == 0) {
                int count = 10;
                if (strlen(input) > 5) {
                    if (input[5] == ' ') {
                        const char* param = input + 6;
                        bool valid = true;
                        for (size_t i = 0; param[i] != '\0'; i++) {
                            if (!std::isdigit(param[i])) {
                                valid = false;
                                break;
                            }
                        }
                        if (valid && strlen(param) > 0) {
                            count = std::stoi(param);
                            if (count <= 0) {
                                std::cout << "Error: N must be a positive number." << std::endl;
                                continue;
                            }
                            if (count > 100) {
                                std::cout << "Warning: Limiting to 100 ping requests" << std::endl;
                                count = 100;
                            }
                        } else {
                            std::cout << "Error: Invalid parameter. Use /ping or /ping <positive_number>" << std::endl;
                            continue;
                        }
                    } else {
                        std::cout << "Error: Invalid command. Use /ping or /ping <number>" << std::endl;
                        continue;
                    }
                }
                pthread_mutex_lock(&ping_mutex);
                ping_records.clear();
                pthread_mutex_unlock(&ping_mutex);

                std::cout << "\n╔══════════════════════════════════════════════════════════════════╗" << std::endl;
                std::cout << "║                         PING TEST                                ║" << std::endl;
                std::cout << "╠══════════════════════════════════════════════════════════════════╣" << std::endl;
                std::cout << "║  Sending " << count << " ping request(s)...                      ║" << std::endl;

                pthread_mutex_lock(&sock_mutex);
                int current_sock = sock;
                pthread_mutex_unlock(&sock_mutex);
                if (current_sock < 0) {
                    std::cout << "║  ERROR: Not connected to server!                                 ║" << std::endl;
                    std::cout << "╚══════════════════════════════════════════════════════════════════╝" << std::endl;
                    continue;
                }
                std::cout << "║  Waiting " << MAX_PING_TIMEOUT << " seconds for responses...     ║" << std::endl;
                std::cout << "╚══════════════════════════════════════════════════════════════════╝" << std::endl;
                uint32_t msg_ids[count];
                for (int i = 0; i < count; i++) {
                    MessageEx ping_msg;
                    memset(&ping_msg, 0, sizeof(MessageEx));
                    strcpy(ping_msg.payload, "ping");
                    ping_msg.length = strlen(ping_msg.payload) + 1;
                    ping_msg.type = MSG_PING;
                    ping_msg.msg_id = current_id++;
                    msg_ids[i] = ping_msg.msg_id;

                    memset(&pending, 0, sizeof(PendingMsg));
                    pending.msg = ping_msg;
                    pending.send_time = time(nullptr);
                    pending.last_send_time = time(nullptr);
                    pending.retries = 0;
                    pthread_mutex_lock(&pendings_mutex);
                    pendings.push_back(pending);
                    pthread_mutex_unlock(&pendings_mutex);
                    usleep(10000); //10ms
                }
                while (!pendings.empty()) {
                    usleep(10000);
                    for (int i = 0; i < count; i++) {
                        pthread_mutex_lock(&pendings_mutex);
                        uint32_t current_id = pendings.front().msg.msg_id;
                        pthread_mutex_unlock(&pendings_mutex);
                        if (msg_ids[i] == current_id) {
                            print_progress_bar(i, count);
                            break;
                        }
                    }
                }
                print_progress_bar(count, count);
                int received = 0;
                double total_rtt = 0;
                double min_rtt = 999999, max_rtt = 0;
                double prev_rtt = 0;
                double total_jitter = 0;
                int jitter_count = 0;
                pthread_mutex_lock(&ping_mutex);
                for (auto& req : ping_records) {
                    if (req.received) {
                        received++;
                        total_rtt += req.rtt;
                        
                        if (req.rtt < min_rtt) min_rtt = req.rtt;
                        if (req.rtt > max_rtt) max_rtt = req.rtt;
                        
                        if (prev_rtt > 0) {
                            double jitter = req.rtt - prev_rtt;
                            if (jitter < 0) jitter = -jitter;
                            total_jitter += jitter;
                        }
                        jitter_count++;
                        prev_rtt = req.rtt;
                    }
                }
                pthread_mutex_unlock(&ping_mutex);
                double loss_percent = (count - received) * 100.0 / count;
                double avg_rtt = (received > 0) ? total_rtt / received : 0;
                double avg_jitter = (jitter_count > 0) ? total_jitter / jitter_count : 0;
                pthread_mutex_lock(&results_mutex);
                last_ping_results.available = true;
                last_ping_results.total_sent = count;
                last_ping_results.received = received;
                last_ping_results.avg_rtt_ms = avg_rtt * 1000;
                last_ping_results.avg_jitter_ms = avg_jitter * 1000;
                last_ping_results.loss_percent = loss_percent;
                last_ping_results.timestamp = time(nullptr);
                pthread_mutex_unlock(&results_mutex);
                std::cout << "\n╔══════════════════════════════════════════════════════════════════╗" << std::endl;
                std::cout << "║                        PING RESULTS                              ║" << std::endl;
                std::cout << "╠══════════════════════════════════════════════════════════════════╣" << std::endl;
                std::cout << "║  Sent:     " << std::setw(5) << count << "                            ║" << std::endl;
                std::cout << "║  Received: " << std::setw(5) << received << " (" << std::fixed << std::setprecision(1) << loss_percent << "%% loss)        ║" << std::endl;
                std::cout << "║  Lost:     " << std::setw(5) << (count - received) << "                                                    ║" << std::endl;
                std::cout << "╠══════════════════════════════════════════════════════════════════╣" << std::endl;
                
                if (received > 0) {
                    std::cout << "║  RTT Statistics:                                               ║" << std::endl;
                    std::cout << "║    Min:  " << std::setw(8) << std::fixed << std::setprecision(3) << min_rtt << "s                                                 ║" << std::endl;
                    std::cout << "║    Max:  " << std::setw(8) << std::fixed << std::setprecision(3) << max_rtt << "s                                                 ║" << std::endl;
                    std::cout << "║    Avg:  " << std::setw(8) << std::fixed << std::setprecision(3) << avg_rtt << "s                                                 ║" << std::endl;
                    std::cout << "║  Jitter: " << std::setw(8) << std::fixed << std::setprecision(3) << avg_jitter << "s                                                 ║" << std::endl;
                }
                
                std::cout << "╚══════════════════════════════════════════════════════════════════╝" << std::endl;
                std::cout << "\nDetailed results:" << std::endl;
                const PingRecord* prev = nullptr;
                for (const auto& req : ping_records) {
                    if (req.received) {
                        std::cout << "  [" << req.msg_id << "] ✓ RTT: " 
                                << std::fixed << std::setprecision(1) << (req.rtt * 1000) << "ms";
                        if (prev != nullptr) {
                            std::cout << " | Jitter: " << std::fixed << std::setprecision(1) 
                                << (fabs(req.rtt - prev->rtt) * 1000) << "ms";
                        }
                        std::cout << std::endl;
                    } else {
                        std::cout << "  [" << req.msg_id << "] ✗ LOST" << std::endl;
                    }
                    prev = &req;
                }
            }
            else if (strcmp(input, "/netdiag") == 0) {
                pthread_mutex_lock(&results_mutex);
                bool has_results = last_ping_results.available;
                PingResults results = last_ping_results;
                pthread_mutex_unlock(&results_mutex);
                if (!has_results) {
                    std::cout << "\n╔══════════════════════════════════════════════════════════════════╗" << std::endl;
                    std::cout << "║                    NETWORK DIAGNOSTICS                           ║" << std::endl;
                    std::cout << "╠══════════════════════════════════════════════════════════════════╣" << std::endl;
                    std::cout << "║  No ping results available!                                     ║" << std::endl;
                    std::cout << "║  Please run /ping first to collect network statistics.         ║" << std::endl;
                    std::cout << "║                                                                ║" << std::endl;
                    std::cout << "║  Usage: /ping [count]  - Run ping test (default: 10 packets)   ║" << std::endl;
                    std::cout << "║  Then:  /netdiag       - Export results to JSON file           ║" << std::endl;
                    std::cout << "╚══════════════════════════════════════════════════════════════════╝" << std::endl;
                    continue;
                }
                std::cout << "\n╔══════════════════════════════════════════════════════════════════╗" << std::endl;
                std::cout << "║                    NETWORK DIAGNOSTICS                           ║" << std::endl;
                std::cout << "╠══════════════════════════════════════════════════════════════════╣" << std::endl;
                std::cout << "║  Based on ping test from: " << format_timestamp(results.timestamp) << "      ║" << std::endl;
                std::cout << "╠══════════════════════════════════════════════════════════════════╣" << std::endl;
                std::cout << "║  RTT avg : " << std::setw(6) << std::fixed << std::setprecision(1) 
                        << results.avg_rtt_ms << " ms" 
                        << std::string(34 - (int)(std::to_string(results.avg_rtt_ms).length() + 7), ' ') << "║" << std::endl;
                std::cout << "║  Jitter  : " << std::setw(6) << std::fixed << std::setprecision(1) 
                        << results.avg_jitter_ms << " ms" 
                        << std::string(34 - (int)(std::to_string(results.avg_jitter_ms).length() + 7), ' ') << "║" << std::endl;
                std::cout << "║  Loss    : " << std::setw(5) << std::fixed << std::setprecision(1) 
                        << results.loss_percent << " %" 
                        << std::string(35 - (int)(std::to_string(results.loss_percent).length() + 3), ' ') << "║" << std::endl;
                std::cout << "╚══════════════════════════════════════════════════════════════════╝" << std::endl;
                save_netdiag_results(nickname, results);
            }
            else if (strncmp(input, "/w ", 3) == 0) {
                char* target = strtok(input + 3, " ");
                char* message = strtok(NULL, "");
                
                if (target && message && strlen(message) > 0) {
                    memset(&pending, 0, sizeof(PendingMsg));
                    snprintf(pending.msg.payload, MAX_PAYLOAD, "%s", message);
                    pending.msg.length = strlen(pending.msg.payload) + 1;
                    pending.msg.type = MSG_PRIVATE;
                    pending.msg.msg_id = current_id++;
                    snprintf(pending.msg.receiver, MAX_NAME, "%s", target);
                    pending.send_time = time(nullptr);
                    pending.last_send_time = time(nullptr);
                    pending.retries = 0;
                    
                    pthread_mutex_lock(&pendings_mutex);
                    pendings.push_back(pending);
                    pthread_mutex_unlock(&pendings_mutex);
                } else {
                    std::cout << "Usage: /w <nickname> <message>" << std::endl;
                }
            }
            else if (strcmp(input, "/help") == 0) {
                std::cout << "\n╔══════════════════════════════════════════════════════════════════╗\n";
                std::cout << "║                      AVAILABLE COMMANDS                          ║\n";
                std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";
                std::cout << "║  /help                              - Show this help message     ║\n";
                std::cout << "║  /list                              - Show online users list     ║\n";
                std::cout << "║  /history                           - Show all last messages     ║\n";
                std::cout << "║  /history N                         - Show last N messages       ║\n";
                std::cout << "║  /quit                              - Disconnect from server     ║\n";
                std::cout << "║  /w <nick> <message>                - Send private message       ║\n";
                std::cout << "║  /ping                              - Receive pong from server   ║\n";
                std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";
                std::cout << "║  Tip: packets never sleep                                        ║\n";
                std::cout << "╚══════════════════════════════════════════════════════════════════╝\n\n";
            }
            else if (strcmp(input, "/list") == 0) {
                strcpy(msg.payload, "list");
                msg.length = strlen(msg.payload) + 1;
                msg.type = MSG_LIST;
                msg.msg_id = current_id++;
                
                pthread_mutex_lock(&sock_mutex);
                if (sock >= 0) {
                    send_message(sock, msg);
                }
                pthread_mutex_unlock(&sock_mutex);
            }
            else if (strncmp(input, "/history", 8) == 0) {
                if (strlen(input) == 8) {
                    strcpy(msg.payload, "");
                    msg.length = 1;
                    msg.type = MSG_HISTORY;
                    msg.msg_id = current_id++;
                    pthread_mutex_lock(&sock_mutex);
                    if (sock >= 0) {
                        send_message(sock, msg);
                    }
                    pthread_mutex_unlock(&sock_mutex);
                }
                else if (strlen(input) > 8 && input[8] == ' ') {
                    const char* param = input + 9;
                    bool valid = true;
                    for (size_t i = 0; param[i] != '\0'; i++) {
                        if (!std::isdigit(param[i])) {
                            valid = false;
                            break;
                        }
                    }
                    if (valid && strlen(param) > 0) {
                        int n = std::stoi(param);
                        if (n > 0) {
                            strcpy(msg.payload, param);
                            msg.length = strlen(msg.payload) + 1;
                            msg.type = MSG_HISTORY;
                            msg.msg_id = current_id++;
                            pthread_mutex_lock(&sock_mutex);
                            if (sock >= 0) {
                                send_message(sock, msg);
                            }
                            pthread_mutex_unlock(&sock_mutex);
                        } else {
                            std::cout << "Error: N must be a positive number." << std::endl;
                            continue;
                        }
                    } else {
                        std::cout << "Error: Invalid parameter. Use /history or /history <positive_number>" << std::endl;
                        continue;
                    }
                } else if (input[0] == '/') {
                    std::cout << "Unknown command";
                    continue;
                }
                else {
                    std::cout << "Error: Use /history or /history <positive_number>" << std::endl;
                    continue;
                }
            }
            else {
                memset(&pending, 0, sizeof(PendingMsg));
                strncpy(pending.msg.payload, input, MAX_PAYLOAD - 1);
                pending.msg.payload[MAX_PAYLOAD-1] = '\0';
                pending.msg.length = strlen(pending.msg.payload) + 1;
                pending.msg.type = MSG_TEXT;
                pending.msg.msg_id = current_id++;
                pending.send_time = time(nullptr);
                pending.last_send_time = time(nullptr);
                pending.retries = 0;
                
                pthread_mutex_lock(&pendings_mutex);
                pendings.push_back(pending);
                pthread_mutex_unlock(&pendings_mutex);
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