#ifndef STRUCTS_H
#define STRUCTS_H

#include <stdint.h>
#include <arpa/inet.h>
#include <cstring>

#define MAX_PAYLOAD 1024
#define MAX_NICKNAME 32

typedef struct
{
    uint32_t length;  // длина поля type + payload
    uint8_t  type;    // тип сообщения
    char     payload[MAX_PAYLOAD]; // данные
} Message;

struct ClientInfo {
    int socket;
    struct sockaddr_in address;
    char nickname[MAX_NICKNAME];
    bool authenticated;
    bool active;
    
    ClientInfo() : socket(-1), authenticated(false), active(false) {
        memset(&address, 0, sizeof(address));
        memset(nickname, 0, sizeof(nickname));
    }
};

enum
{
    MSG_HELLO   = 1,  // клиент -> сервер (ник)
    MSG_WELCOME = 2,  // сервер -> клиент
    MSG_TEXT    = 3,  // текст
    MSG_PING    = 4,
    MSG_PONG    = 5,
    MSG_BYE     = 6,

    MSG_AUTH        = 7,  // аутентификация
    MSG_PRIVATE     = 8,  // личное сообщение
    MSG_ERROR       = 9,  // ошибка
    MSG_SERVER_INFO = 10  // системные сообщения
};

#endif