#include "client.h"

#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

void add_client_to_list(client_t *client, client_list_t *client_list){
    client->prev = NULL;
    client->next = client_list->head;
    client_list->head = client;
    if (NULL != client->next) 
        client->next->prev = client;
}

void create_client(client_list_t *client_list, int client_sockfd){
    client_t *new_client;
    new_client = calloc(1, sizeof(client_t));
    if (NULL == new_client){
        if (ERROR_LOG) perror("create_client: Unable to allocate memory");
        close(client_sockfd);
        return;
    }
    if(-1 == client_init(new_client, client_sockfd)){
        close(client_sockfd);
        return;
    }
    add_client_to_list(new_client, client_list);
    if (INFO_LOG) printf("[%d] Connected\n", client_sockfd);
}

int client_init(client_t *client, int client_sockfd){
    client->sockfd = client_sockfd;
    client->status = AWAITING_REQUEST;
    client->cache_node = NULL;
    client->http_entry = NULL;
    client->bytes_written = 0;
    client->request = NULL;
    client->request_size = 0;
    client->request_alloc_size = 0;

    if(-1 == fcntl(client_sockfd, F_SETFL, O_NONBLOCK)){
        if (ERROR_LOG) perror("client error: fcntl error");
    }
    return 0;
}

void client_remove(client_t *client, client_list_t *client_list){
    if(client == client_list->head){
        client_list->head = client->next;
        if(NULL != client_list->head){
            client_list->head->prev = NULL;
        }
    }
    else{
        client->prev->next = client->next;
        if(NULL != client->next){
            client->next->prev = client->prev;
        }
    }
    if (INFO_LOG) printf("[%d] Disconnected\n", client->sockfd);
    client_destroy(client);
    free(client);
}

void client_destroy(client_t *client){
    if(NULL != client->http_entry){
        client->http_entry->clients--;
    }
    close(client->sockfd);
}

void client_spam_error(client_t *client){
    client->status = SOCK_ERROR;
    if (client->http_entry != NULL){
        client->http_entry->clients--;
        client->http_entry = NULL;
    }
    client->bytes_written = 0;
    client->request_size = 0;
    free(client->request);
    client->request = NULL;
}

void client_update_http_info(client_t *client){
    if(NULL != client->http_entry){
        if(IS_ERROR_STATUS(client->http_entry->status)){
            client_spam_error(client);
        }
        else if (client->http_entry->cache_node != NULL && client->http_entry->cache_node->is_full){
            client->status = GETTING_FROM_CACHE;
            client->cache_node = client->http_entry->cache_node;
            client->http_entry->clients--;
            client->http_entry = NULL;
        }
    }
}

void check_finish_write_to_client(client_t *client){
    size_t size = 0;
    if(client->status == GETTING_FROM_CACHE){ 
        size = client->cache_node->size;

    } else if (client->status == DOWNLOADING){
        size = client->http_entry->data_size; //если скачиваем берем размер http
    }
    
    if(client->bytes_written >= size && 
                ((client->status == GETTING_FROM_CACHE && client->cache_node->is_full) ||
                (client->status == DOWNLOADING && client->http_entry->is_response_complete))){
        client->bytes_written = 0;

        client->cache_node = NULL;
        if(NULL != client->http_entry){
            client->http_entry->clients--;
            client->http_entry = NULL;
        }

        client->status = AWAITING_REQUEST;
    }
}

int parse_client_request(client_t *client, char **host, char **path, ssize_t bytes_read) {
    const char *method, *phr_path;
    size_t method_len, path_len;
    int minor_version;
    struct phr_header headers[100];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);

    int err_code = phr_parse_request(client->request, client->request_size, &method, &method_len, &phr_path, &path_len, &minor_version, headers, &num_headers, client->request_size - bytes_read);
    if (err_code == -1) {
         if (ERROR_LOG) fprintf(stderr, "parse_client_request: unable to parse request\n");
        client_spam_error(client);
        return -1;
    }
    if (err_code == -2) return -2; //incomplete, read from client more

    if (!strings_equal_by_length(method, method_len, "GET", 3)) {
         if (ERROR_LOG) fprintf(stderr, "parse_client_request: not a GET method\n");
        client_spam_error(client);
        return -1;
    }

    *path = (char *)calloc(path_len + 1, sizeof(char));
    if (*path == NULL) {
         if (ERROR_LOG) fprintf(stderr, "parse_client_request: unable to allocate memory for path\n");
        client_spam_error(client);
        return -1;
    }
    memcpy(*path, phr_path, path_len);

    int found_host = 0;
    for (size_t i = 0; i < num_headers; i++) {
        if (strings_equal_by_length(headers[i].name, headers[i].name_len,  "Host", 4)) {
            *host = calloc(headers[i].value_len + 1, sizeof(char));
            if (*host == NULL) {
                if (ERROR_LOG) fprintf(stderr, "parse_client_request: unable to allocate memory for host\n");
                free(*path);
                *path = NULL;
                client_spam_error(client);
                return -1;
            }
            memcpy(*host, headers[i].value, headers[i].value_len);
            found_host = 1;
            break;
        }
    }
    if (!found_host) {
        if (ERROR_LOG) fprintf(stderr, "parse_client_request: no host header\n");
        free(*path);
        *path = NULL;
        client_spam_error(client);
        return -1;
    }

    return 0;
}

void handle_client_request(client_t *client, ssize_t bytes_read, http_list_t *http_list, cache_t *cache) {
    char *host = NULL, *path = NULL;
    int errorCode = parse_client_request(client, &host, &path, bytes_read);
    if (-1 == errorCode) {
        client_spam_error(client);
        return;
    }
    if (-2 == errorCode) 
        return; //неполный запрос  

    client->request_alloc_size = 0;

    cache_node_t *entry = cache_find(host, path, cache);
    if (NULL != entry && entry->is_full) { //если мы его нашли и он полный то мы говорим клиенту чтобы он брал данные из кэша
        printf("[%d] Getting data from cache for '%s%s'\n", client->sockfd, host, path);
        client->status = GETTING_FROM_CACHE;
        client->cache_node = entry;
        client->request_size = 0;
        free(client->request);
        client->request = NULL;
        free(host); 
        free(path);
        return;
    }

    http_t *http_entry = http_list->head; //если мы кэш не нашли или он неполный
    while (NULL != http_entry) {    //пытаемся найти существующее соединение которое у нас есть
        if (STR_EQ(http_entry->host, host) && STR_EQ(http_entry->path, path) &&
                (http_entry->status == DOWNLOADING || http_entry->status == SOCK_DONE)) {
            client->request_size = 0;
            free(client->request);
            client->request = NULL;
            http_entry->clients++;
            break;
        }
        http_entry = http_entry->next;
    }

    if (http_entry == NULL) {  //если нет активного соединения то мы его создаем
        int http_sock_fd = http_open_socket(host, 80);
        if (-1 == http_sock_fd) {
            client_spam_error(client);
            free(host); 
            free(path);
            return;
        }

        http_entry = create_http(http_sock_fd, client->request, client->request_size, host, path, http_list);
        if (http_entry == NULL) {
            client_spam_error(client);
            free(host); 
            free(path);
            close(http_sock_fd);
            return;
        }

        client->request_size = 0;
        client->request = NULL;
    }

    client->status = DOWNLOADING;
    client->http_entry = http_entry;
}

void client_read_data(client_t *client, http_list_t *http_list, cache_t *cache) {
    char buf[BUF_SIZE + 1]; 
    errno = 0;
    ssize_t bytes_read = recv(client->sockfd, buf, BUF_SIZE, MSG_DONTWAIT); //принимаем запрос от клиента
    if (-1 == bytes_read){
        if (errno == EWOULDBLOCK) 
            return;
        perror("client_read_data: Unable to read from client socket");
        client_spam_error(client);
        return;
    }
    if (bytes_read == 0) {  //если от клиента нет запроса
        client->status = SOCK_DONE; //закончил
        client->request_size = 0;
        free(client->request);     //удаляем
        client->request = NULL;            
        return;
    }

    if (client->status != AWAITING_REQUEST) {     
        if ((client->status == DOWNLOADING && client->bytes_written == client->http_entry->data_size) || //если все получил из запроса 
            (client->status == GETTING_FROM_CACHE && client->bytes_written == client->cache_node->size)) {  //если получил из кеша все
            if (client->http_entry != NULL) {
                client->http_entry->clients--;
                client->http_entry = NULL;
            }
            client->bytes_written = 0;
            client->status = AWAITING_REQUEST;
            client->request_size = 0;
            client->request_alloc_size = 0;
            free(client->request);     //удаляем
            client->request = NULL; 
        }
    }

    if (client->request_size + bytes_read > client->request_alloc_size) {
        client->request_alloc_size += BUF_SIZE;
        char *check = (char *)realloc(client->request, client->request_alloc_size);
        if (NULL == check) {
            perror("read_data_from_client: Unable to reallocate memory for client request");
            client_spam_error(client);
            return;
        }
        client->request = check;
    }

    memcpy(client->request + client->request_size, buf, bytes_read);
    client->request_size += bytes_read;

    handle_client_request(client, bytes_read, http_list, cache);
}

void check_finished_writing_to_client(client_t *client) {
    size_t size = 0;

    if (client->status == GETTING_FROM_CACHE) size = client->cache_node->size;
    else if (client->status == DOWNLOADING) size = client->http_entry->data_size;

    if (client->bytes_written >= size && (
            (client->status == GETTING_FROM_CACHE && client->cache_node->is_full) ||
            (client->status == DOWNLOADING && client->http_entry->is_response_complete))) {
        client->bytes_written = 0;

        client->cache_node = NULL;
        if (client->http_entry != NULL) {
            client->http_entry->clients--;
            client->http_entry = NULL;
        }

        client->status = AWAITING_REQUEST;
    }
}

void write_to_client(client_t *client) {
    ssize_t offset = client->bytes_written;
    const char *buf = "";
    ssize_t size = 0;

    if (client->status == GETTING_FROM_CACHE) {
        buf = client->cache_node->data;
        size = client->cache_node->size;
    }
    else if (client->status == DOWNLOADING) {
        buf = client->http_entry->data;
        size = client->http_entry->data_size;
    }

    ssize_t bytes_written = write(client->sockfd, buf + offset, size - offset);
    if (bytes_written == -1) {
        if (errno == EWOULDBLOCK) return;
        perror("write_to_client: Unable to write to client socket");
        client_spam_error(client);
        return;
    }
    client->bytes_written += bytes_written;

    check_finished_writing_to_client(client);
}