#include "client.h"
#include "proxyServer.h"
#include "errorPrinter.h"
#include "states.h"

#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

void add_client_to_list(client_t *client, client_list_t *client_list){ //вставляем клиента в начало списка
    rwlock_wrlock(&client_list->rwlock, "add_client_to_list");

    client->prev = NULL;
    client->next = client_list->head;
    client_list->head = client;
    if (NULL != client->next) 
        client->next->prev = client;
        
    rwlock_unlock(&client_list->rwlock, "add_client_to_list");
}

void create_client(client_list_t *client_list, int client_sockfd){ //создание клиента
    client_t *new_client;
    int errorCode;
    new_client = calloc(1, sizeof(client_t));
    if (NULL == new_client){
        perror("create_client: Unable to allocate memory");
        close(client_sockfd);
        return;
    }
    if(-1 == client_init(new_client, client_sockfd)){ 
        close(client_sockfd);
        return;
    }

    errorCode = pthread_create(&new_client->pthread_client, NULL, client_thread, new_client);

    if (0 != errorCode){
        print_error("create_client error: Unable to create thread", errorCode);
        free(new_client);
        close(client_sockfd);
        return; 
    }
}

int client_init(client_t *client, int client_sockfd){ //присваиваем клиенту сокет и статус
    client->sockfd = client_sockfd;  //accept вернул сокет подключившегося клиент
    client->status = AWAITING_REQUEST; //ждём запрос
    client->cache_node = NULL;
    client->http_entry = NULL;
    client->bytes_written = 0;
    client->request = NULL;
    client->request_size = 0;
    client->request_alloc_size = 0;
    return 0;
}

void client_remove(client_t *client, client_list_t *client_list){   //удаление клиента из листа клиентов
    rwlock_wrlock(&client_list->rwlock, "client_remove");

    if (client == client_list->head){   //если это вершина списка
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
    rwlock_unlock(&client_list->rwlock, "client_remove");

    printf("[%d] Disconnected\n", client->sockfd);
    client_destroy(client); //уничтожаем клиента
    free(client); //очищаем память под данного клиента
}

void client_destroy(client_t *client){
    if(NULL != client->http_entry){   //если у него был запрос
        lock_mutex(&client->http_entry->mutex, "client_destroy");
        client->http_entry->clients--;  //то снимаем с этого запроса клиента
        unlock_mutex(&client->http_entry->mutex, "client_destroy");
        cond_broadcast(&client->http_entry->cond, "client_destroy");
    }
    close(client->sockfd);   //закрываем сокет клиента
}

void client_spam_error(client_t *client){ //в случае ошибки со стороны запроса
    client->status = SOCK_ERROR; //ставим что у клиента ошибка
    if (NULL != client->http_entry){ //если у него был запрос то удаляем клиентский запрос а также удаляем этого клиента из запроса
        lock_mutex(&client->http_entry->mutex, "client_spam_error");
        client->http_entry->clients--;
        unlock_mutex(&client->http_entry->mutex, "client_spam_error");
        cond_broadcast(&client->http_entry->cond, "client_destroy");
        client->http_entry = NULL;
    }
    client->bytes_written = 0;  //очищаем кол-во записанных клиентом данных
    client->request_size = 0; //зануляем длину запроса
    free(client->request);  //обнуляем запрос
    client->request = NULL; 
}

void client_update_http_info(client_t *client){
    if(NULL != client->http_entry){
        lock_mutex(&client->http_entry->mutex, "client_update_http_info");
        if(IS_ERROR_STATUS(client->http_entry->status)){ //если у клиента ошибка с его http запросом
            unlock_mutex(&client->http_entry->mutex, "client_update_http_info");
            client_spam_error(client);
            return;
        }
        //если сервер полкчил все данные по запросу
        else if (NULL != client->http_entry->cache_node && client->http_entry->cache_node->is_full){
            client->status = GETTING_FROM_CACHE; //ставим сатус получения кеша для клиента
            client->cache_node = client->http_entry->cache_node;  //копируем кэш из запроса в клиента
            client->http_entry->clients--;  //убираем клиента из списка по этому запросу
            unlock_mutex(&client->http_entry->mutex, "client_update_http_info");
            client->http_entry = NULL;
            return;
        }
        unlock_mutex(&client->http_entry->mutex, "client_update_http_info");
    }
}

int parse_client_request(client_t *client, char **host, char **path, ssize_t bytes_read) { //парсим запрос клиента
    const char *method, *phr_path; //метод, адрес ресурса в ссылке
    size_t method_len, path_len; //длина метода, длина ресура в ссылке
    int minor_version;
    struct phr_header headers[100];     //заголовки на страницы
    size_t num_headers = 100;  //длина хэдеров

    int errorCode = phr_parse_request(client->request, client->request_size, &method, &method_len, &phr_path, &path_len, &minor_version, headers, &num_headers, client->request_size - bytes_read);
    if (-1 == errorCode){
        fprintf(stderr, "parse_client_request: unable to parse request\n");
        client_spam_error(client);
        return -1;
    }
    if (-2 == errorCode) 
        return -2; //неполный запрос

    if (!strings_equal_by_length(method, method_len, "GET", 3)) { //если это нет GET method - удаляем запрос
        fprintf(stderr, "parse_client_request: not a GET method\n");
        client_spam_error(client);
        return -1;
    }

    *path = (char *)calloc(path_len + 1, sizeof(char)); //выделяем данные под адрес ресурса
    if (*path == NULL) {
        fprintf(stderr, "parse_client_request: unable to allocate memory for path\n");
        client_spam_error(client);
        return -1;
    }
    memcpy(*path, phr_path, path_len);

    int found_host = 0;
    for (size_t i = 0; i < num_headers; i++) {
        if (strings_equal_by_length(headers[i].name, headers[i].name_len,  "Host", 4)) {
            *host = calloc(headers[i].value_len + 1, sizeof(char));
            if (*host == NULL) {
                fprintf(stderr, "parse_client_request: unable to allocate memory for host\n");
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
        fprintf(stderr, "parse_client_request: no host header\n");
        free(*path);
        *path = NULL;
        client_spam_error(client);
        return -1;
    }

    return 0;
}

void handle_client_request(client_t *client, ssize_t bytes_read, http_list_t *http_list, cache_t *cache) { //обрабатываем запрос клиента
    char *host = NULL, *path = NULL;
    int errorCode = parse_client_request(client, &host, &path, bytes_read); //парсим запрос
    if (-1 == errorCode) {
        client_spam_error(client);
        return;
    }
    if (-2 == errorCode) 
        return; //неполный запрос  

    client->request_alloc_size = 0;
    
    cache_node_t *entry = cache_find(host, path, cache);
    int is_full = 0;
    if(NULL != entry){
        rwlock_rdlock(&entry->rwlock, "handle_client_request");
        is_full = entry->is_full;
        rwlock_unlock(&entry->rwlock, "handle_client_request");
    }
    
    if (NULL != entry && is_full) { //если мы его нашли и он полный то мы говорим клиенту чтобы он брал данные из кэша
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
    
    lock_mutex(&http_list->mutex, "handle_client_request");

    http_t *http_entry = http_list->head; //если его ещё нет в кеше
    while (NULL != http_entry) {    //пытаемся найти существующее соединение которое мб у нас есть
        lock_mutex(&http_entry->mutex, "handle_client_request");
        if (STR_EQ(http_entry->host, host) && STR_EQ(http_entry->path, path) &&
                (http_entry->status == DOWNLOADING || http_entry->status == SOCK_DONE)) {
            http_entry->clients++;
            unlock_mutex(&http_entry->mutex, "handle_client_request");
            client->request_size = 0;
            free(client->request);
            client->request = NULL;
            break;
        }
        unlock_mutex(&http_entry->mutex, "handle_client_request");
        http_entry = http_entry->next;
    }
    

    if (http_entry == NULL) {  //если нет активного соединения то мы его создаем
        int http_sock_fd = http_open_socket(host, 80); //создаем сокет запроса на 80 порте
        if (-1 == http_sock_fd) {
            unlock_mutex(&http_list->mutex, "handle_client_request");
            client_spam_error(client);
            free(host); 
            free(path);            
            return;
        }

        http_entry = create_http(http_sock_fd, client->request, client->request_size, host, path, http_list); //создаем соединение с хостом
        if (NULL == http_entry) {
            unlock_mutex(&http_list->mutex, "handle_client_request");
            client_spam_error(client);
            free(host); 
            free(path);
            close(http_sock_fd);
            return;
        }

        client->request_size = 0;
        client->request = NULL;
    }
    unlock_mutex(&http_list->mutex, "handle_client_request");

    client->status = DOWNLOADING;
    client->http_entry = http_entry; //присваиваем клиенту этот запрос
}

void client_read_data(client_t *client, http_list_t *http_list, cache_t *cache) { //клиент считывает данные из http соединения в свой кэш
    char buf[BUF_SIZE];
    errno = 0;
    ssize_t bytes_read = recv(client->sockfd, buf, BUF_SIZE, 0);
    if (-1 == bytes_read){
        perror("client_read_data: Unable to read from client socket");
        client_spam_error(client);
        return;
    }
    if (bytes_read == 0) {  //если соединения по этому клиенту закрылось (больше нет принятый данных)
        client->status = SOCK_DONE; //ставим статус "закончил"
        client->request_size = 0;
        free(client->request);     //удаляем
        client->request = NULL;            
        return;
    }

    if (client->request_size + bytes_read > client->request_alloc_size) {  //чекаем превысили ли мы размер запроса
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
    int is_complete = 0;
    
    if (client->status == GETTING_FROM_CACHE){ //если клиент получает данные из кэша запроса
        rwlock_rdlock(&client->cache_node->rwlock, "check_finished_writing_to_client");
        size = client->cache_node->size; //берем кэш из клиента
        is_complete = client->cache_node->is_full;
        rwlock_unlock(&client->cache_node->rwlock, "check_finished_writing_to_client");
    } 
    else if (client->status == DOWNLOADING){
        lock_mutex(&client->http_entry->mutex, "check_finished_writing_to_client"); 
        size = client->http_entry->data_size; //если скачивает, то берем кэш-данные из его запроса
        is_complete = client->http_entry->is_response_complete;
        unlock_mutex(&client->http_entry->mutex, "check_finished_writing_to_client");
    }
    if (client->bytes_written >= size &&   //если записали все данные
            (client->status == GETTING_FROM_CACHE || client->status == DOWNLOADING) && is_complete){
        client->bytes_written = 0;

        client->cache_node = NULL;
        if (NULL != client->http_entry) {
            lock_mutex(&client->http_entry->mutex, "check_finished_writing_to_client");
            client->http_entry->clients--;
            cond_broadcast(&client->http_entry->cond, "check_finished_writing_to_client");
            unlock_mutex(&client->http_entry->mutex, "check_finished_writing_to_client");
            client->http_entry = NULL;
        }

        client->status = AWAITING_REQUEST;
    }
}

void write_to_client(client_t *client) { //отправляем клиенту то что прочитали из кэша
    ssize_t offset = client->bytes_written;
    char buf[BUF_SIZE];
    ssize_t size = 0;

    if (client->status == GETTING_FROM_CACHE){
        rwlock_rdlock(&client->cache_node->rwlock, "write_to_client");
        size = MIN(client->cache_node->size - offset, BUF_SIZE);
        memcpy(buf, client->cache_node->data + offset, size);
        rwlock_unlock(&client->cache_node->rwlock, "write_to_client");
    }
    else if (client->status == DOWNLOADING){
        lock_mutex(&client->http_entry->mutex, "write_to_client");
        size = MIN(client->http_entry->data_size - offset, BUF_SIZE);
        memcpy(buf, client->http_entry->data + offset, size);
        unlock_mutex(&client->http_entry->mutex, "write_to_client");
    }

    ssize_t bytes_written = send(client->sockfd, buf, size, 0); 
    if (bytes_written == -1) {
        perror("write_to_client: Unable to write to client socket");
        client_spam_error(client);
        return;
    }
    
    client->bytes_written += bytes_written;

    check_finished_writing_to_client(client);
}