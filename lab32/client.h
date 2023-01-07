#ifndef LAB31_CLIENT_H

#include "cache.h"
#include "http.h"
#include "states.h"
#include "picohttpparser.h"
#include <semaphore.h>

#include <pthread.h>

#define LAB31_CLIENT_H

typedef struct client{
    pthread_t pthread_client;
    sem_t sem;
    ssize_t cur_allowed_size;
    time_t last_send_time;
    int sockfd, status; //status - состояние клиента
    cache_node_t *cache_node; //данные который получает клиент
    http_t *http_entry; //запрос клиента
    char *request; //сам запрос
    ssize_t request_size; //размер запроса
    ssize_t request_alloc_size; //кол во памяти выделенное под запрос
    ssize_t bytes_written; //кол-во записанных байт
    struct client *prev, *next;
} client_t;

typedef struct client_list{
    size_t length;
    client_t *head;
    pthread_rwlock_t rwlock;    
} client_list_t;

void create_client(client_list_t *client_list, int client_sockfd);
void add_client_to_list(client_t *client, client_list_t *client_list);
int client_init(client_t *client, int client_sockfd);
void client_add_to_list(client_t *client, client_list_t *client_list);
void client_remove(client_t *client, client_list_t *client_list);

void client_update_http_info(client_t *cur_client);
void check_finished_writing_to_client(client_t *client);
void client_spam_error(client_t *client);
void check_finish_write_to_client(client_t *client);

void client_destroy(client_t *client);
void client_read_data(client_t *client, http_list_t *http_list, cache_t *cache);
void write_to_client(client_t *client, size_t length);

#endif //LAB31_CLIENT_H
