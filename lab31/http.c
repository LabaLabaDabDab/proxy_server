#include "http.h"
#include "states.h"
#include "cache.h"

#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

int convert_number(char *str, int *number) {
    errno = 0;
    char *endptr = "";
    long num = strtol(str, &endptr, 10);

    if (errno != 0) {
        perror("Can't convert given number");
        return -1;
    }
    if (strcmp(endptr, "") != 0) {
        fprintf(stderr, "Number contains invalid symbols\n");
        return -1;
    }

    *number = (int)num;
    return 0;
}

int get_number_from_string_by_length(const char *str, size_t length) {
    char buf1[length + 1];
    memcpy(buf1, str, length);
    buf1[length] = '\0';
    int num = -1;
    convert_number(buf1, &num);
    return num;
}

void http_add_to_list(http_t *http, http_list_t *http_list) {
    http->prev = NULL;
    http->next = http_list->head;
    http_list->head = http;
    if (NULL != http->next) 
        http->next->prev = http;
}

http_t *create_http(int sock_fd, char *request, ssize_t request_size, char *host, char *path, http_list_t *http_list) {
    http_t *new_http = (http_t *)calloc(1, sizeof(http_t));
    if (NULL == new_http) {
        perror("create_http: Unable to allocate memory for http struct");
        return NULL;
    }
    if (-1 == http_init(new_http, sock_fd, request, request_size, host, path)) {
        free(new_http);
        return NULL;
    }
    http_add_to_list(new_http, http_list);
    return new_http;
}

int http_init(http_t *http, int sockfd, char *request, ssize_t request_size, char *host, char *path) {
    http->status = AWAITING_REQUEST;
    http->clients = 1;  //we create http if there is a request, so we already have 1 client
    http->data = NULL; 
    http->data_size = 0;
    http->code = HTTP_CODE_UNDEFINED;
    http->headers_size = HTTP_NO_HEADERS;
    http->response_type = HTTP_RESPONSE_NONE;
    http->is_response_complete = 0;
    http->decoder.consume_trailer = 1;
    http->sockfd = sockfd;
    http->request = request;
    http->request_size = request_size;
    http->response_alloc_size = 0;
    http->request_bytes_written = 0;
    http->host = host; 
    http->path = path;
    http->cache_node = NULL;
    return 0;
}

int http_open_socket(const char *hostname, int port) {
    char array[6] = {0};
    sprintf(array, "%d", port);
    int errorCode;
    struct addrinfo *result;
    errorCode = getaddrinfo(hostname, array, NULL, &result);
    if (0 != errorCode){
        fprintf(stderr, "Unable to connect to host %s: %s\n", hostname, gai_strerror(errorCode));
        return -1;
    }

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == sock_fd) {
        perror("open_http_socket: socket error");
        return -1;
    }

    if (connect(sock_fd, result->ai_addr, sizeof(struct sockaddr_in)) == -1) {
        perror("open_http_socket: connect error");
        close(sock_fd);
        return -1;
    }

    if (-1 == fcntl(sock_fd, F_SETFL, O_NONBLOCK)) {
        perror("open_http_socket: fcntl error");
    }
    freeaddrinfo(result);

    return sock_fd;
}

void http_remove_from_list(http_t *http, http_list_t *http_list) {
    if (http == http_list->head) {
        http_list->head = http->next;
        if (NULL != http_list->head) 
            http_list->head->prev = NULL;
    }
    else {
        http->prev->next = http->next;
        if (NULL !=http->next)
            http->next->prev = http->prev;
    }
}

void close_socket(int *sockfd){
    if(0 > *sockfd){
        return;
    }
    close(*sockfd);
    *sockfd = -1; //если потом будем использовать не удаляем на него указатель
}

int http_check_disconnect(http_t *http){
    if(0 == http->clients){        //если у нас нет клиентов которые подавали данный запрос
        if(IS_ERROR_OR_DONE_STATUS(http->status)){ //чекаем получили ли ответ или сдохли
            return 1;
        }
        #ifdef DROP_HTTP_NO_CLIENTS  //на случай если все клиенты решат рипнуться 
            close_socket(&http->sockfd);
            return 1;
        #endif             
    }
    return 0;    
}

void http_destroy(http_t *http, cache_t *cache){ //уничтожаем запрос
    if (http->cache_node != NULL && !http->cache_node->is_full){ //если кеш не полный
        cache_remove(http->cache_node, cache);
        http->cache_node = NULL;
    }
    else if(NULL == http->cache_node){
        free(http->data);
        free(http->host);
        free(http->path);
    }
    close_socket(&http->sockfd); 
}

void remove_http(http_t *http, http_list_t *http_list, cache_t *cache){
    http_remove_from_list(http, http_list);
    printf("[%d %s %s] Disconnected\n", http->sockfd, http->host, http->path);
    http_destroy(http, cache);
    free(http);
}

void http_spam_error(http_t *http) {
    http->status = SOCK_ERROR;
    close_socket(&http->sockfd);
    http->data_size = 0;
    http->is_response_complete = 0;
}

void parse_http_response_headers(http_t *http) {
    int minor_version, status;
    const char *msg;
    size_t msg_len;
    struct phr_header headers[100];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);

    int headers_size = phr_parse_response(http->data, http->data_size, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);
    if (-1 == headers_size) {
        fprintf(stderr, "parse_http_response: Unable to parse http response headers\n");
        http_spam_error(http);
        return;
    }

    if (0 != status) 
        http->code = status; //request may be incomplete, but it managed to get status
    if (headers_size >= 0) 
        http->headers_size = headers_size;

    http->response_type = HTTP_RESPONSE_NONE;
    for (int i = 0; i < num_headers; i++) {
        if (strings_equal_by_length(headers[i].name, headers[i].name_len, "Transfer-Encoding", strlen("Transfer-Encoding")) &&
        strings_equal_by_length(headers[i].value, headers[i].value_len, "chunked", strlen("chunked"))) {
            http->response_type = HTTP_RESPONSE_CHUNKED;
        }
        if (strings_equal_by_length(headers[i].name, headers[i].name_len, "Content-Length", strlen("Content-Length"))) {
            http->response_type = HTTP_RESPONSE_CONTENT_LENGTH;
            http->response_size = get_number_from_string_by_length(headers[i].value, headers[i].value_len);
            if (-1 == http->response_size) {
                http_spam_error(http);
                return;
            }
        }
    }
}

void parse_http_response_chunked(http_t *entry, char *buf, ssize_t offset, ssize_t size, cache_t *cache) {
    size_t rsize = size;
    ssize_t pret;

    pret = phr_decode_chunked(&entry->decoder, buf + offset, &rsize);
    if (-1 == pret) {
        fprintf(stderr, "parse_http_response_chunked: Unable to parse response\n");
        http_spam_error(entry);
        return;
    }

    if (entry->code == 200) {
        if (NULL == entry->cache_node) {
            entry->cache_node = cache_add(entry->host, entry->path, entry->data, entry->data_size, cache);
            if (NULL == entry->cache_node) 
                entry->code = HTTP_CODE_NONE;
        }
        else {
            entry->cache_node->data = entry->data;
            entry->cache_node->size = entry->data_size;
        }
    }

    if (0 == pret) {
        if (entry->cache_node != NULL) entry->cache_node->is_full = 1;
        entry->is_response_complete = 1;
    }
}

void parse_http_response_by_length(http_t *entry, cache_t *cache) {
    if (entry->code == 200) {
        if (entry->cache_node == NULL) {
            entry->cache_node = cache_add(entry->host, entry->path, entry->data, entry->data_size, cache);
            if (entry->cache_node == NULL) entry->code = HTTP_CODE_NONE;
        }
        else {
            entry->cache_node->data = entry->data;
            entry->cache_node->size = entry->data_size;
        }
    }
    if (entry->data_size == entry->headers_size + entry->response_size) {
        if (entry->cache_node != NULL) entry->cache_node->is_full = 1;
        entry->is_response_complete = 1;
    }
}

void http_read_data(http_t *entry, cache_t *cache) {
    char buf[BUF_SIZE];
    errno = 0;
    ssize_t bytes_read = recv(entry->sockfd, buf, BUF_SIZE, MSG_DONTWAIT);
    if (-1 == bytes_read) {
        if (errno == EWOULDBLOCK) 
        return;
        perror("http_read_data: Unable to read from http socket");
        http_spam_error(entry);
        return;
    }
    if (0 == bytes_read) {
        entry->status = SOCK_DONE;
        if (entry->response_type == HTTP_RESPONSE_NONE) {
            entry->is_response_complete = 1;
            if (NULL != entry->cache_node) 
                entry->cache_node->is_full = 1;
        }
        close_socket(&entry->sockfd);
        return;
    }

    if (entry->status != DOWNLOADING) {
        fprintf(stderr, "http_read_data: reading from http when we shouldn't\n");
        return;
    }

    if (entry->data_size + bytes_read > entry->response_alloc_size) {
        entry->response_alloc_size += BUF_SIZE;
        char *check = (char *)realloc(entry->data, entry->response_alloc_size);
        if (check == NULL) {
            perror("http_read_data: Unable to reallocate memory for http data");
            http_spam_error(entry);
            return;
        }
        entry->data = check;
    }

    memcpy(entry->data + entry->data_size, buf, bytes_read);
    entry->data_size += bytes_read;

    int b_no_headers = entry->headers_size == HTTP_NO_HEADERS; //чекаем есть хэдеры или нет 1- есть 0 - нет
    if (entry->headers_size == HTTP_NO_HEADERS) parse_http_response_headers(entry); 
    if (entry->status == SOCK_ERROR) return;

    if (entry->headers_size >= 0) {
        if (entry->response_type == HTTP_RESPONSE_CHUNKED) {
            parse_http_response_chunked(entry, buf, b_no_headers ? entry->headers_size : 0, b_no_headers ? entry->data_size - entry->headers_size : bytes_read, cache);
        }
        else if (entry->response_type == HTTP_RESPONSE_CONTENT_LENGTH) {
            parse_http_response_by_length(entry, cache);
        }
    }
}

void http_send_request(http_t *entry) {
    ssize_t bytes_written = write(entry->sockfd, entry->request + entry->request_bytes_written, entry->request_size - entry->request_bytes_written);
    if (bytes_written >= 0) entry->request_bytes_written += bytes_written;
    if (entry->request_bytes_written == entry->request_size) {
        entry->status = DOWNLOADING;
        entry->request_size = 0;
        free(entry->request);
        entry->request = NULL;
    }
    if (bytes_written == -1) {
        perror("http_send_request: unable to write to http socket");
        http_spam_error(entry);
    }
}