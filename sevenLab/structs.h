#ifndef STRUCTS_H
#define STRUCTS_H

#include <stdint.h>
#include <arpa/inet.h>
#include <cstring>
#include <string>
#include <sstream>

#define MAX_NAME     32
#define MAX_PAYLOAD  256
#define MAX_TIME_STR 32
#define MAX_TIMEOUT_SECONDS 2
#define MAX_PING_TIMEOUT 2
#define MAX_ATTEMPS_RESEND 3

//для парсинга из файла
struct HistoryMessage {
    uint32_t msg_id;
    time_t timestamp;
    std::string sender;
    std::string receiver;
    std::string type;
    std::string text;
    bool delivered;
    bool is_offline;
};

struct PingResults {
    bool available = false;
    int total_sent = 0;
    int received = 0;
    double avg_rtt_ms = 0.0;
    double avg_jitter_ms = 0.0;
    double loss_percent = 0.0;
    time_t timestamp = 0;
};

typedef struct {
    uint32_t length;                 // длина полезной части
    uint8_t  type;                   // тип сообщения
    uint32_t msg_id;                 // уникальный идентификатор сообщения
    char     sender[MAX_NAME];       // ник отправителя
    char     receiver[MAX_NAME];     // ник получателя или "" если используется broadcast
    time_t   timestamp;              // время создания
    char     payload[MAX_PAYLOAD];   // текст / данные команды
} MessageEx;

typedef struct {  
    char sender[MAX_NAME];  
    char receiver[MAX_NAME];  
    char text[MAX_PAYLOAD];  
    time_t timestamp;  
    uint32_t msg_id;  
} OfflineMsg;

typedef struct {  
    MessageEx msg;  
    time_t send_time;  
    time_t last_send_time;
    int retries;  
} PendingMsg;

struct PingRecord {
    uint32_t msg_id;
    time_t send_time;
    double rtt;
    bool received;
};

struct ClientInfo {
    int socket;
    struct sockaddr_in address;
    char nickname[MAX_NAME];
    uint32_t last_ids[32];
    bool authenticated;
    bool active;
    int last_ids_start;
    int last_ids_count;
    void* ssl;
    
    ClientInfo() : socket(-1), authenticated(false), active(false),
                   last_ids_start(0), last_ids_count(0) {
        memset(&address, 0, sizeof(address));
        memset(nickname, 0, sizeof(nickname));
        memset(last_ids, 0, sizeof(last_ids));
    }
};

enum {
    MSG_HELLO        = 1,
    MSG_WELCOME      = 2,
    MSG_TEXT         = 3,
    MSG_PING         = 4,
    MSG_PONG         = 5,
    MSG_BYE          = 6,

    MSG_AUTH         = 7,
    MSG_PRIVATE      = 8,
    MSG_ERROR        = 9,
    MSG_SERVER_INFO  = 10,

    MSG_LIST         = 11,
    MSG_HISTORY      = 12,
    MSG_HISTORY_DATA = 13,
    MSG_HELP         = 14,

    MSG_ACK          = 15,

    MSG_TLS_INFO     = 16,
    MSG_SECURE_ERROR = 17
};

#endif