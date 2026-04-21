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

struct ClientInfo {
    int socket;
    struct sockaddr_in address;
    char nickname[MAX_NAME];
    bool authenticated;
    bool active;
    
    ClientInfo() : socket(-1), authenticated(false), active(false) {
        memset(&address, 0, sizeof(address));
        memset(nickname, 0, sizeof(nickname));
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
	
	MSG_LIST         = 11,  // список пользователей
    MSG_HISTORY      = 12,  // запрос истории
    MSG_HISTORY_DATA = 13,  // ответ с историей
    MSG_HELP         = 14   // справочная информация
};

#endif