#include <cstdint>
#include <cstdio>
#include <iostream>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <atomic>
#include <sstream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <csignal>
#include <vector>
#include <queue>
#include <algorithm>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <getopt.h>
#include "structs.h"

#define PORT 8080
#define THREAD_POOL_SIZE 10
#define MAX_QUEUE_SIZE 100
#define MAX_CLIENTS 100
#define MAX_HISTORY 100
#define LOG_FILE "messages_log.json"

bool keepRunning = true;
std::atomic<std::uint32_t> id{0};

int network_delay_ms = 0;
double drop_probability = 0.0;
double corrupt_probability = 0.0;

std::vector<ClientInfo> clients;
std::vector<OfflineMsg> offline;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t offline_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t json_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

std::queue<int> client_queue;

SSL_CTX* ssl_ctx = nullptr;

void handleSignal(int /*signal*/) {
    std::cout << "\n[Security][TLS] Shutting down server..." << std::endl;
    exit(0);
}

void init_openssl() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    std::cout << "[Security][TLS] OpenSSL initialized" << std::endl;
}

void cleanup_openssl() {
    if (ssl_ctx) {
        SSL_CTX_free(ssl_ctx);
    }
    EVP_cleanup();
    ERR_free_strings();
}

bool create_ssl_context(const char* cert_file, const char* key_file) {
    ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!ssl_ctx) {
        std::cerr << "[Security][TLS] Failed to create SSL context" << std::endl;
        ERR_print_errors_fp(stderr);
        return false;
    }
    
    if (SSL_CTX_use_certificate_file(ssl_ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {
        std::cerr << "[Security][CERT] Failed to load certificate: " << cert_file << std::endl;
        ERR_print_errors_fp(stderr);
        return false;
    }
    std::cout << "[Security][CERT] certificate loaded: " << cert_file << std::endl;
    
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_file, SSL_FILETYPE_PEM) <= 0) {
        std::cerr << "[Security][CERT] Failed to load private key: " << key_file << std::endl;
        ERR_print_errors_fp(stderr);
        return false;
    }
    std::cout << "[Security][CERT] private key loaded: " << key_file << std::endl;
    
    if (!SSL_CTX_check_private_key(ssl_ctx)) {
        std::cerr << "[Security][CERT] Private key does not match certificate" << std::endl;
        return false;
    }
    
    return true;
}

void parse_args(int argc, char* argv[]) {
    const char* cert_file = "server.crt";
    const char* key_file = "server.key";
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg.find("--delay=") == 0) {
            network_delay_ms = std::stoi(arg.substr(8));
            std::cout << "[SIM] Network delay enabled: " << network_delay_ms << " ms" << std::endl;
        }
        else if (arg.find("--drop=") == 0) {
            drop_probability = std::stod(arg.substr(7));
            std::cout << "[SIM] Packet drop enabled: " << drop_probability * 100 << "%" << std::endl;
        }
        else if (arg.find("--corrupt=") == 0) {
            corrupt_probability = std::stod(arg.substr(10));
            std::cout << "[SIM] Packet corruption enabled: " << corrupt_probability * 100 << "%" << std::endl;
        }
        else if (arg.find("--cert=") == 0) {
            cert_file = argv[i] + 7;
        }
        else if (arg.find("--key=") == 0) {
            key_file = argv[i] + 6;
        }
        else {
            std::cerr << "Unknown parameter: " << arg << std::endl;
            std::cerr << "Usage: " << argv[0] << " [--delay=ms] [--drop=probability] [--corrupt=probability] [--cert=file] [--key=file]" << std::endl;
        }
    }
    
    if (!create_ssl_context(cert_file, key_file)) {
        exit(1);
    }
}

void log_layer(int layer, const std::string& message) {
    std::cout << "[Layer " << layer << " - ";
    switch(layer) {
        case 2: std::cout << "Internet"; break;
        case 3: std::cout << "Transport"; break;
        case 4: std::cout << "Application"; break;
        default: std::cout << "Unknown";
    }
    std::cout << "] " << message << std::endl;
}

void log_transport_send(const std::string& msg_type, uint32_t msg_id) {
    log_layer(3, "send " + msg_type + " (id=" + std::to_string(msg_id) + ")");
}

void log_transport_recv(const std::string& msg_type, uint32_t msg_id) {
    log_layer(3, "recv " + msg_type + " (id=" + std::to_string(msg_id) + ")");
}

bool ssl_send_message(SSL* ssl, const MessageEx& msg) {
    if (!ssl) return false;
    std::string msg_type;
    switch(msg.type) {
        case MSG_ACK: msg_type = "MSG_ACK"; break;
        case MSG_TEXT: msg_type = "MSG_TEXT"; break;
        case MSG_PRIVATE: msg_type = "MSG_PRIVATE"; break;
        case MSG_PING: msg_type = "MSG_PING"; break;
        case MSG_PONG: msg_type = "MSG_PONG"; break;
        case MSG_WELCOME: msg_type = "MSG_WELCOME"; break;
        case MSG_ERROR: msg_type = "MSG_ERROR"; break;
        case MSG_BYE: msg_type = "MSG_BYE"; break;
        case MSG_AUTH: msg_type = "MSG_AUTH"; break;
        case MSG_LIST: msg_type = "MSG_LIST"; break;
        case MSG_HISTORY: msg_type = "MSG_HISTORY"; break;
        case MSG_HISTORY_DATA: msg_type = "MSG_HISTORY_DATA"; break;
        case MSG_SERVER_INFO: msg_type = "MSG_SERVER_INFO"; break;
        default: msg_type = "UNKNOWN"; break;
    }
    
    log_transport_send(msg_type, msg.msg_id);
    MessageEx net_msg = msg;
    net_msg.length = htonl(msg.length);
    net_msg.msg_id = htonl(msg.msg_id);
    
    int ret = SSL_write(ssl, &net_msg, sizeof(MessageEx));
    if (ret <= 0) {
        int err = SSL_get_error(ssl, ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            return false;
        }
    }
    return true;
}

bool ssl_recv_message(SSL* ssl, MessageEx& msg, bool& dropped) {
    dropped = false;
    if (!ssl) return false;
    
    int ret = SSL_read(ssl, &msg, sizeof(MessageEx));
    if (ret <= 0) {
        int err = SSL_get_error(ssl, ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            return false;
        }
        return false;
    }
    
    msg.length = ntohl(msg.length);
    msg.msg_id = ntohl(msg.msg_id);

    std::string msg_type;
    switch(msg.type) {
        case MSG_ACK: msg_type = "MSG_ACK"; break;
        case MSG_TEXT: msg_type = "MSG_TEXT"; break;
        case MSG_PRIVATE: msg_type = "MSG_PRIVATE"; break;
        case MSG_PING: msg_type = "MSG_PING"; break;
        case MSG_PONG: msg_type = "MSG_PONG"; break;
        case MSG_WELCOME: msg_type = "MSG_WELCOME"; break;
        case MSG_ERROR: msg_type = "MSG_ERROR"; break;
        case MSG_BYE: msg_type = "MSG_BYE"; break;
        case MSG_AUTH: msg_type = "MSG_AUTH"; break;
        case MSG_LIST: msg_type = "MSG_LIST"; break;
        case MSG_HISTORY: msg_type = "MSG_HISTORY"; break;
        case MSG_HISTORY_DATA: msg_type = "MSG_HISTORY_DATA"; break;
        case MSG_SERVER_INFO: msg_type = "MSG_SERVER_INFO"; break;
        default: msg_type = "UNKNOWN"; break;
    }
    
    log_transport_recv(msg_type, msg.msg_id);
    
    if (network_delay_ms > 0) {
        std::cout << "[Transport][SIM] DELAY applied: " << network_delay_ms << " ms" << std::endl;
        usleep(network_delay_ms * 1000);
    }
    
    if (drop_probability > 0.0) {
        double random = (double)rand() / RAND_MAX;
        if (random < drop_probability) {
            std::cout << "[Transport][SIM] DROP (id=" << msg.msg_id 
                      << ", rate=" << drop_probability << ")" << std::endl;
            dropped = true;
            return true;
        }
    }
    
    if (corrupt_probability > 0.0) {
        double random = (double)rand() / RAND_MAX;
        if (random < corrupt_probability) {
            int payload_len = strlen(msg.payload);
            if (payload_len > 0) {
                int pos = rand() % payload_len;
                char original = msg.payload[pos];
                do {
                    msg.payload[pos] = (char)((rand() % 256) - 128);
                } while (msg.payload[pos] == original && payload_len > 1);
                
                std::cout << "[Transport][SIM] CORRUPT payload (id=" << msg.msg_id 
                          << ", pos=" << pos << ")" << std::endl;
            }
        }
    }
    
    return true;
}

std::string unescape_json(const std::string& s) {
    std::string result;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: result += s[i + 1];
            }
            i++;
        } else {
            result += s[i];
        }
    }
    return result;
}

std::string extract_string(const std::string& line, const std::string& key) {
    std::string pattern = "\"" + key + "\":\"";
    size_t start = line.find(pattern);
    if (start == std::string::npos) return "";

    start += pattern.length();
    size_t i = start;

    while (i < line.size()) {
        if (line[i] == '"' && line[i - 1] != '\\') break;
        i++;
    }

    return unescape_json(line.substr(start, i - start));
}

long extract_number(const std::string& line, const std::string& key) {
    std::string pattern = "\"" + key + "\":";
    size_t start = line.find(pattern);
    if (start == std::string::npos) return 0;

    start += pattern.length();
    size_t end = line.find_first_of(",}", start);

    return std::stol(line.substr(start, end - start));
}

std::string escape_json(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    return result;
}

bool update_delivered(uint32_t msg_id) {
    pthread_mutex_lock(&json_mutex);

    std::ifstream file(LOG_FILE);
    if (!file.is_open()) {
        pthread_mutex_unlock(&json_mutex);
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    bool updated = false;

    while (std::getline(file, line)) {
        uint32_t id = extract_number(line, "msg_id");

        if (id == msg_id) {
            size_t pos = line.find("\"delivered\":false");
            if (pos != std::string::npos) {
                line.replace(pos, strlen("\"delivered\":false"), "\"delivered\":true");
                updated = true;
            }
        }

        lines.push_back(line);
    }

    file.close();

    std::ofstream out(LOG_FILE, std::ios::trunc);
    for (auto& l : lines) {
        out << l << "\n";
    }
    out.close();

    pthread_mutex_unlock(&json_mutex);
    return updated;
}

uint32_t load_max_id() {
    pthread_mutex_lock(&json_mutex);

    std::ifstream file(LOG_FILE);
    if (!file.is_open()) {
        pthread_mutex_unlock(&json_mutex);
        return 0;
    }

    uint32_t max_id = 0;
    std::string line;

    while (std::getline(file, line)) {
        uint32_t id = extract_number(line, "msg_id");
        if (id > max_id) {
            max_id = id;
        }
    }

    file.close();
    pthread_mutex_unlock(&json_mutex);

    return max_id;
}

void log_message_to_json(const MessageEx& msg, bool is_offline) {
    pthread_mutex_lock(&json_mutex);
    std::ofstream file(LOG_FILE, std::ios::app);
    if (!file.is_open()) {
        pthread_mutex_unlock(&json_mutex);
        return;
    }
    
    file << "{";
    file << "\"msg_id\":" << msg.msg_id << ",";
    file << "\"timestamp\":" << msg.timestamp << ",";
    file << "\"sender\":\"" << escape_json(msg.sender) << "\",";
    file << "\"receiver\":\"" << escape_json(msg.receiver) << "\",";
    file << "\"type\":\"" << (msg.type == MSG_PRIVATE ? "MSG_PRIVATE" : "MSG_TEXT") << "\",";
    file << "\"text\":\"" << escape_json(msg.payload) << "\",";
    file << "\"delivered\":" << (is_offline ? "false" : "true") << ",";
    file << "\"is_offline\":" << (is_offline ? "true" : "false");
    file << "}\n";
    file.close();
    pthread_mutex_unlock(&json_mutex);
}

std::string format_timestamp(time_t timestamp) {
    struct tm* tm_info = localtime(&timestamp);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buffer);
}

std::string get_history(int n, const std::string& requesting_user) {
    std::vector<HistoryMessage> all_messages;
    pthread_mutex_lock(&json_mutex);
    
    std::ifstream file(LOG_FILE);
    if (!file.is_open()) {
        pthread_mutex_unlock(&json_mutex);
        return "No history available.\n";
    }
    
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        HistoryMessage msg;
        msg.msg_id = extract_number(line, "msg_id");
        msg.timestamp = extract_number(line, "timestamp");
        msg.sender = extract_string(line, "sender");
        msg.receiver = extract_string(line, "receiver");
        msg.type = extract_string(line, "type");
        msg.text = extract_string(line, "text");
        std::string str_bool = extract_string(line, "is_offline");
        std::transform(str_bool.begin(), str_bool.end(), str_bool.begin(), ::tolower);
        msg.is_offline = (str_bool == "true");

        all_messages.push_back(msg);
    }
    file.close();
    pthread_mutex_unlock(&json_mutex);

    std::ostringstream result;
    int total = all_messages.size();
    int start = (n > 0 && n < total) ? total - n : 0;

    for (int i = start; i < total; i++) {
        const auto& msg = all_messages[i];
        std::string time_str = format_timestamp(msg.timestamp);
        bool is_private = (msg.type == "MSG_PRIVATE");
        bool is_for_requester = (msg.receiver == requesting_user || msg.sender == requesting_user);
        
        result << "[" << time_str << "]";
        result << "[id=" << msg.msg_id << "]";
        if (msg.is_offline) {
            if (is_for_requester) {
                result << "[OFFLINE][" << msg.sender << " -> " << msg.receiver << "]: ";
                result << msg.text;
            }
            else {
                result << "[OFFLINE][" << msg.sender << " -> " << msg.receiver << "]: ";
                result << std::string(msg.text.length(), '*');
            }
        }
        else if (is_private) {
            if (is_for_requester) {
                result << "[PRIVATE][" << msg.sender << " -> " << msg.receiver << "]: ";
                result << msg.text;
            }
            else {
                result << "[PRIVATE][" << msg.sender << " -> " << msg.receiver << "]: ";
                result << std::string(msg.text.length(), '*');
            }
        } else {
            result << "[" << msg.sender << "]: " << msg.text;
        }
        result << "\n";
    }
    
    if (result.str().empty()) {
        return "No messages in history.\n";
    }
    return result.str();
}

ClientInfo* find_client_by_ssl(SSL* ssl) {
    for (auto& client : clients) {
        if (client.ssl == ssl) {
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

std::string get_online_users_list() {
    std::string result = "[SERVER]: Online users\n";
    
    pthread_mutex_lock(&clients_mutex);
    for (const auto& client : clients) {
        if (client.authenticated && client.active && strlen(client.nickname) > 0) {
            result += "  ";
            result += client.nickname;
            result += "\n";
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    return result;
}

int parse_history_param(const char* payload) {
    if (payload == nullptr || payload[0] == '\0') {
        return -1;
    }
    char* endptr;
    long val = strtol(payload, &endptr, 10);
    if (endptr == payload || *endptr != '\0' || val <= 0) {
        return 0;
    }
    if (val > MAX_HISTORY) {
        val = MAX_HISTORY;
    }
    return (int)val;
}

bool is_nickname_unique(const std::string& nickname) {
    for (const auto& client : clients) {
        if (client.authenticated && std::string(client.nickname) == nickname) {
            return false;
        }
    }
    return true;
}

bool is_message_processed(ClientInfo* client, uint32_t msg_id) {
    for (int i = 0; i < client->last_ids_count; i++) {
        int index = (client->last_ids_start + i) % 32;
        if (client->last_ids[index] == msg_id) {
            return true;
        }
    }
    return false;
}

void add_processed_id(ClientInfo* client, uint32_t msg_id) {
    if (client->last_ids_count < 32) {
        int index = (client->last_ids_start + client->last_ids_count) % 32;
        client->last_ids[index] = msg_id;
        client->last_ids_count++;
    } else {
        client->last_ids[client->last_ids_start] = msg_id;
        client->last_ids_start = (client->last_ids_start + 1) % 32;
    }
}

void broadcast_message(const MessageEx& msg, SSL* sender_ssl) {
    log_layer(4, "broadcast message");
    
    pthread_mutex_lock(&clients_mutex);
    
    ClientInfo* sender = find_client_by_ssl(sender_ssl);
    if (!sender || !sender->authenticated) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }
    
    for (const auto& client : clients) {
        if (client.ssl != sender_ssl && client.authenticated && client.ssl) {
            if (!ssl_send_message((SSL*)client.ssl, msg)) {
                std::cout << "Failed to send to client " << client.nickname << std::endl;
            }
        }
    }
    
    pthread_mutex_unlock(&clients_mutex);
}

void send_private_message(const std::string& target_nick, const std::string& message, SSL* sender_ssl) {
    log_layer(4, "private message started");
    
    pthread_mutex_lock(&clients_mutex);
    ClientInfo* sender = find_client_by_ssl(sender_ssl);
    if (!sender || !sender->authenticated) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }
    ClientInfo* target = find_client_by_nickname(target_nick);
    pthread_mutex_unlock(&clients_mutex);
    
    MessageEx private_msg;
    memset(&private_msg, 0, sizeof(MessageEx));
    snprintf(private_msg.payload, MAX_PAYLOAD, "%s", message.c_str());
    private_msg.length = strlen(private_msg.payload) + 1;
    private_msg.type = MSG_PRIVATE;
    private_msg.msg_id = id.fetch_add(1);
    snprintf(private_msg.sender, MAX_NAME, "%s", sender->nickname);
    snprintf(private_msg.receiver, MAX_NAME, "%s", target_nick.c_str());
    private_msg.timestamp = time(nullptr);
    
    if (!target) {
        log_layer(4, "offline private message save to send later");
        OfflineMsg offline_msg;
        memset(&offline_msg, 0, sizeof(OfflineMsg));
        snprintf(offline_msg.text, MAX_PAYLOAD, "%s", private_msg.payload);
        snprintf(offline_msg.sender, MAX_NAME, "%s", private_msg.sender);
        snprintf(offline_msg.receiver, MAX_NAME, "%s", private_msg.receiver);
        offline_msg.timestamp = private_msg.timestamp;
        offline_msg.msg_id = private_msg.msg_id;

        pthread_mutex_lock(&offline_mutex);
        offline.push_back(offline_msg);
        pthread_mutex_unlock(&offline_mutex);

        log_layer(4, "save offline message to file");
        log_message_to_json(private_msg, true);
    }
    else if (!ssl_send_message((SSL*)target->ssl, private_msg)) {
        log_layer(4, "fail send message");
        MessageEx error_msg;
        memset(&error_msg, 0, sizeof(MessageEx));
        snprintf(error_msg.payload, MAX_PAYLOAD, "Failed to send message to '%s'", target_nick.c_str());
        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;
        ssl_send_message(sender_ssl, error_msg);
    } else {
        log_layer(4, "success private message");
        std::cout << "[PRIVATE] " << sender->nickname << " -> " << target_nick << ": " << message << std::endl;
        log_layer(4, "save online message to file");
        log_message_to_json(private_msg, false);
    }
}

bool authenticate_client(SSL* ssl, const std::string& nickname) {
    log_layer(4, "authentication process started");
    
    if (nickname.empty()) {
        log_layer(4, "authentication failed: empty nickname");
        MessageEx error_msg;
        memset(&error_msg, 0, sizeof(MessageEx));
        strcpy(error_msg.payload, "Nickname cannot be empty");
        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;
        ssl_send_message(ssl, error_msg);
        return false;
    }
    
    if (!is_nickname_unique(nickname)) {
        log_layer(4, "authentication failed: nickname not unique");
        MessageEx error_msg;
        memset(&error_msg, 0, sizeof(MessageEx));
        snprintf(error_msg.payload, MAX_PAYLOAD, "Nickname '%s' is already taken", nickname.c_str());
        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;
        ssl_send_message(ssl, error_msg);
        return false;
    }
    
    ClientInfo* client = find_client_by_ssl(ssl);
    if (!client) {
        log_layer(4, "authentication failed: client not found");
        return false;
    }
    
    strncpy(client->nickname, nickname.c_str(), MAX_NAME - 1);
    client->nickname[MAX_NAME - 1] = '\0';
    client->authenticated = true;
    
    log_layer(4, "authentication success: " + nickname);
    
    MessageEx welcome_msg;
    memset(&welcome_msg, 0, sizeof(MessageEx));
    snprintf(welcome_msg.payload, MAX_PAYLOAD, "Welcome %s! Total users: %zu", 
             nickname.c_str(), clients.size());
    welcome_msg.length = strlen(welcome_msg.payload) + 1;
    welcome_msg.type = MSG_WELCOME;
    ssl_send_message(ssl, welcome_msg);
    
    MessageEx info_msg;
    memset(&info_msg, 0, sizeof(MessageEx));
    snprintf(info_msg.payload, MAX_PAYLOAD, "User [%s] connected", nickname.c_str());
    info_msg.length = strlen(info_msg.payload) + 1;
    info_msg.type = MSG_SERVER_INFO;

    for (const auto& c : clients) {
        if (c.ssl != ssl && c.authenticated && c.ssl) {
            ssl_send_message((SSL*)c.ssl, info_msg);
        }
    }
    
    std::cout << "User [" << nickname << "] connected" << std::endl;
    return true;
}

void remove_client(SSL* ssl) {
    log_layer(4, "remove client");
    pthread_mutex_lock(&clients_mutex);
    
    auto it = std::find_if(clients.begin(), clients.end(), 
        [ssl](const ClientInfo& c) { return c.ssl == ssl; });
    
    if (it != clients.end()) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &it->address.sin_addr, ip, INET_ADDRSTRLEN);
        
        if (it->authenticated) {
            MessageEx info_msg;
            memset(&info_msg, 0, sizeof(MessageEx));
            info_msg.type = MSG_SERVER_INFO;
            snprintf(info_msg.payload, MAX_PAYLOAD, "User [%s] disconnected", it->nickname);
            info_msg.length = sizeof(info_msg.type) + strlen(info_msg.payload) + 1;
            
            for (const auto& client : clients) {
                if (client.ssl != ssl && client.authenticated && client.ssl) {
                    ssl_send_message((SSL*)client.ssl, info_msg);
                }
            }
            
            std::cout << "User [" << it->nickname << "] disconnected" << std::endl;
        }
        
        if (it->ssl) {
            SSL_shutdown((SSL*)it->ssl);
            SSL_free((SSL*)it->ssl);
        }
        close(it->socket);
        clients.erase(it);
    }
    
    pthread_mutex_unlock(&clients_mutex);
}

void* handle_client(void* arg) {
    int client_sock = *(int*)arg;
    free(arg);
    
    log_layer(4, "new client connection handler started");
    
    std::cout << "[Transport] TCP connection accepted" << std::endl;
    
    SSL* ssl = SSL_new(ssl_ctx);
    if (!ssl) {
        std::cerr << "[Security][TLS] Failed to create SSL object" << std::endl;
        close(client_sock);
        return nullptr;
    }
    
    std::cout << "[Security][TLS] SSL object created" << std::endl;
    SSL_set_fd(ssl, client_sock);
    
    std::cout << "[Security][TLS] handshake started" << std::endl;
    if (SSL_accept(ssl) <= 0) {
        std::cerr << "[Security][TLS] handshake failed" << std::endl;
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        close(client_sock);
        return nullptr;
    }
    
    std::cout << "[Security][TLS] handshake success" << std::endl;
    std::cout << "[Security][ENC] encrypted channel established" << std::endl;
    
    ClientInfo client;
    client.socket = client_sock;
    client.ssl = ssl;
    client.authenticated = false;
    client.active = true;
    client.last_ids_start = 0;
    client.last_ids_count = 0;
    memset(client.last_ids, 0, sizeof(client.last_ids));
    socklen_t addr_len = sizeof(client.address);
    getpeername(client_sock, (struct sockaddr*)&client.address, &addr_len);
    
    pthread_mutex_lock(&clients_mutex);
    clients.push_back(client);
    pthread_mutex_unlock(&clients_mutex);
    
    MessageEx msg;
    bool dropped = true;
    
    if (!ssl_recv_message(ssl, msg, dropped) || dropped || msg.type != MSG_AUTH) {
        log_layer(4, "authentication required, closing connection");
        MessageEx error_msg;
        memset(&error_msg, 0, sizeof(MessageEx));
        strcpy(error_msg.payload, "Authentication required");
        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;
        ssl_send_message(ssl, error_msg);
        remove_client(ssl);
        return nullptr;
    }
    
    log_layer(4, "received MSG_AUTH, payload: " + std::string(msg.payload));
    
    if (!authenticate_client(ssl, msg.payload)) {
        log_layer(4, "authentication failed, closing connection");
        remove_client(ssl);
        return nullptr;
    }

    pthread_mutex_lock(&offline_mutex);
    ClientInfo* auth_client = find_client_by_ssl(ssl);
    if (!auth_client) {
        pthread_mutex_unlock(&offline_mutex);
        log_layer(4, "auth_client not found");
        remove_client(ssl);
        return nullptr;
    }
    
    for (auto it = offline.begin(); it != offline.end(); ) {
        OfflineMsg& offline_msg = *it;
        if (strcmp(offline_msg.receiver, auth_client->nickname) == 0) {
            MessageEx msg;
            memset(&msg, 0, sizeof(MessageEx));
            snprintf(msg.payload, MAX_PAYLOAD, "[OFFLINE]%s", offline_msg.text);
            msg.length = strlen(msg.payload) + 1;
            msg.type = MSG_PRIVATE;
            snprintf(msg.sender, MAX_NAME, "%s", offline_msg.sender);
            snprintf(msg.receiver, MAX_NAME, "%s", offline_msg.receiver);
            msg.timestamp = offline_msg.timestamp;
            msg.msg_id = offline_msg.msg_id;
            if (ssl_send_message(ssl, msg)) {
                log_layer(4, "Offline message delivered");
                update_delivered(offline_msg.msg_id);
                it = offline.erase(it);
            }
            else {
                log_layer(4, "Failed to deliver offline message");
                ++it;
            }
        }
        else {
            ++it;
        }
    }
    pthread_mutex_unlock(&offline_mutex);
    
    while (keepRunning && client.active) {
        dropped = false;
        if (!ssl_recv_message(ssl, msg, dropped)) {
            log_layer(4, "lost connection with client");
            break;
        }

        if (dropped) {
            log_layer(4, "problem message from client");
            continue;
        }
        
        log_layer(4, "processing message type: " + std::to_string(msg.type));
        ClientInfo* sender = find_client_by_ssl(ssl);
        if (!sender) {
            log_layer(4, "sender not found");
            break;
        }
        
        if (is_message_processed(sender, msg.msg_id)) {
            log_layer(4, "duplicate message detected, ignoring. ID: " + std::to_string(msg.msg_id));
            continue;
        }
        
        std::string sender_nickname = sender->nickname;
        std::string target = msg.receiver;
        std::string message = msg.payload;
        std::string user_list = get_online_users_list();
        int n = 0;
        uint32_t prev_id = msg.msg_id;
        MessageEx ack;
        
        switch (msg.type) {
            case MSG_TEXT:
                log_layer(4, "broadcasting text message: " + std::string(msg.payload));
                std::cout << "[" << sender_nickname
                          << "]: " << msg.payload << std::endl;
                
                MessageEx broadcast_msg;
                memset(&broadcast_msg, 0, sizeof(MessageEx));
                snprintf(broadcast_msg.payload, MAX_PAYLOAD, "%s", message.c_str());
                broadcast_msg.length = strlen(broadcast_msg.payload) + 1;
                broadcast_msg.type = MSG_TEXT;
                snprintf(broadcast_msg.sender, MAX_NAME, "%s", sender_nickname.c_str());
                broadcast_msg.timestamp = time(nullptr);
                broadcast_msg.msg_id = id.fetch_add(1);
                
                broadcast_message(broadcast_msg, ssl);
                log_message_to_json(broadcast_msg, false);
                log_layer(3, "send MSG_ACK (" + std::to_string(msg.msg_id) + ")");
                add_processed_id(sender, msg.msg_id);
                ack.type = MSG_ACK;
                ack.msg_id = prev_id;
                ssl_send_message(ssl, ack);
                break;
                
            case MSG_PRIVATE:
                log_layer(4, "processing private message");
                send_private_message(target, message, ssl);
                log_layer(3, "send MSG_ACK (" + std::to_string(msg.msg_id) + ")");
                add_processed_id(sender, msg.msg_id);
                ack.type = MSG_ACK;
                ack.msg_id = prev_id;
                ssl_send_message(ssl, ack);
                break;

            case MSG_LIST:
                log_layer(4, "processing message list");
                add_processed_id(sender, msg.msg_id);
                MessageEx info_msg;
                memset(&info_msg, 0, sizeof(MessageEx));
                snprintf(info_msg.payload, MAX_PAYLOAD, "%s", user_list.c_str());
                info_msg.length = strlen(info_msg.payload) + 1;
                info_msg.type = MSG_SERVER_INFO;
                ssl_send_message(ssl, info_msg);
                break;
            
            case MSG_HISTORY:
                log_layer(4, "processing message history");
                n = parse_history_param(msg.payload);
                add_processed_id(sender, msg.msg_id);
                if (n == 0) {
                    log_layer(4, "invalid history parameter");
                    MessageEx error_msg;
                    memset(&error_msg, 0, sizeof(MessageEx));
                    snprintf(error_msg.payload, MAX_PAYLOAD, 
                            "Invalid parameter. Use /history or /history <positive_number>");
                    error_msg.length = strlen(error_msg.payload) + 1;
                    error_msg.type = MSG_ERROR;
                    ssl_send_message(ssl, error_msg);
                    break;
                }
                MessageEx history_msg;
                memset(&history_msg, 0, sizeof(MessageEx));
                snprintf(history_msg.payload, MAX_PAYLOAD, "%s", get_history(n, sender_nickname).c_str());
                history_msg.length = strlen(history_msg.payload) + 1;
                history_msg.type = MSG_HISTORY_DATA;
                ssl_send_message(ssl, history_msg);
                break;
                
            case MSG_PING:
                log_layer(4, "responding to PING with PONG");
                MessageEx pong_msg;
                memset(&pong_msg, 0, sizeof(MessageEx));
                pong_msg.type = MSG_PONG;
                pong_msg.msg_id = prev_id;
                ssl_send_message(ssl, pong_msg);
                log_layer(3, "send MSG_ACK (" + std::to_string(msg.msg_id) + ")");
                add_processed_id(sender, msg.msg_id);
                ack.type = MSG_ACK;
                ack.msg_id = prev_id;
                ssl_send_message(ssl, ack);
                break;
                
            case MSG_BYE:
                log_layer(4, "client requested disconnect");
                client.active = false;
                break;
                
            default:
                log_layer(4, "unknown message type: " + std::to_string(msg.type));
                MessageEx error_msg;
                memset(&error_msg, 0, sizeof(MessageEx));
                snprintf(error_msg.payload, MAX_PAYLOAD, "%s", "unknown message type");
                error_msg.length = strlen(error_msg.payload) + 1;
                error_msg.type = MSG_ERROR;
                ssl_send_message(ssl, error_msg);
        }
    }
    
    remove_client(ssl);
    return nullptr;
}

void* worker_thread(void* /*arg*/) {
    while (keepRunning) {
        pthread_mutex_lock(&queue_mutex);
        
        while (client_queue.empty() && keepRunning) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        
        if (!keepRunning && client_queue.empty()) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        
        int* client_sock = new int;
        *client_sock = client_queue.front();
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

int main(int argc, char** argv) {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);
    
    init_openssl();
    parse_args(argc, argv);

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

    id.store(load_max_id() + 1);
    
    std::cout << "[Transport] listening on port " << PORT << std::endl;
    std::cout << "TCP/IP Layer visualization enabled" << std::endl;
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
        log_layer(2, "New connection from " + std::string(client_ip) + ":" + std::to_string(ntohs(client_addr.sin_port)));
        std::cout << "[Transport] TCP connection accepted" << std::endl;
        
        add_to_queue(client_sock);
    }
    
    close(server_sock);
    
    pthread_mutex_lock(&clients_mutex);
    for (auto& client : clients) {
        if (client.ssl) {
            SSL_shutdown((SSL*)client.ssl);
            SSL_free((SSL*)client.ssl);
        }
        close(client.socket);
    }
    clients.clear();
    pthread_mutex_unlock(&clients_mutex);
    
    cleanup_openssl();
    
    std::cout << "Server shutting down" << std::endl;
    return 0;
}