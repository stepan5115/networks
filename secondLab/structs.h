#ifndef STRUCTS_H
#define STRUCTS_H

#include <stdint.h>

#define MAX_PAYLOAD 1024

typedef struct
{
    uint32_t length;  // длина поля type + payload
    uint8_t  type;    // тип сообщения
    char     payload[MAX_PAYLOAD]; // данные
} Message;

enum
{
    MSG_HELLO   = 1,  // клиент -> сервер (ник)
    MSG_WELCOME = 2,  // сервер -> клиент
    MSG_TEXT    = 3,  // текст
    MSG_PING    = 4,
    MSG_PONG    = 5,
    MSG_BYE     = 6
};

#endif