#ifndef LAB31_HTTP_H

#include "cache.h"
#include "picohttpparser.h"

#include <time.h>

#define LAB31_HTTP_H

typedef struct http{                                         //code - ответ запроса        is_response_complete - закончен ли ответ или нет (все ли данные пришли?)
    int sockfd, code, clients, status, is_response_complete; //status - состояние http запроса.  clients - кол-во клиентов которые используют один и тот же http-запрос 
    int response_type, headers_size;                         //response_type = (CHUNKED - мы не знаем длину ответа, поэтому мы получаем запрос по частям) 
                                                             //|| (Content-length - мы знаем длину ответа и мы знаем когда остановиться) 
                                                             //|| NONE (мы не знаем ничего, тогда мы считаем ответ законченным когда разорвется http-соединение)
                                                             //headers_size - кол-во заголовков        
    ssize_t response_size;  //размер ответа
    ssize_t response_alloc_size; //кол-во выделенной памяти под ответ
    struct phr_chunked_decoder decoder; //скомунизженно
    char *data; //сами данные
    ssize_t data_size; //размер данных
    char *request; //сам запрос (переходит от клиента)
    ssize_t request_size; //размер запроса (переходит от клиента)
    ssize_t request_bytes_written;  //кол-во записанных байт
    char *host, *path; //host - куда обращаться //остальная часть url
    cache_node_t *cache_node; //кэш для ответа
    int sockfd_copy; //дополнительный сокет под копирку
    int just_created;
    time_t last_recv_time;
    ssize_t cur_allowed_size;
    struct http *prev, *next;
}http_t;

typedef struct http_list{
    size_t length;
    http_t *head;
}http_list_t;

http_t *create_http(int sock_fd, char *request, ssize_t request_size, char *host, char *path, http_list_t *http_list);
void remove_http(http_t *http, http_list_t *http_list, cache_t *cache);

int http_init(http_t *http, int sock_fd, char *request, ssize_t request_size, char *host, char *path);
void http_destroy(http_t *http, cache_t *cache);

int http_check_disconnect(http_t *http);
int http_open_socket(const char *hostname, int port);

void http_read_data(http_t *entry, cache_t *cache, size_t length);
void http_send_request(http_t *entry);

void close_socket(int *sockfd);

#endif //LAB31_HTTP_H