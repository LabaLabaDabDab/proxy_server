#include "proxyServer.h"
#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <sys/signal.h>
#include <errno.h>

client_list_t client_list = {.head = NULL, .rwlock = PTHREAD_RWLOCK_INITIALIZER};
http_list_t http_list = {.head = NULL, .mutex= PTHREAD_MUTEX_INITIALIZER};
cache_t cache;

volatile int STOPPED_PROGRAMM = 0;

int parse_port(char *port_str, int *port){
    if(-1 == convert_number(port_str, port)){
        perror("parse_port error\n");
        return -1;
    }
    return 0;
}

void remove_all_connections(){
    rwlock_rdlock(&client_list.rwlock, "remove_all_connections");
    client_t *cur_client = client_list.head;
    while (NULL != cur_client){
        client_t *next = cur_client->next;
        pthread_cancel(cur_client->pthread_client);
        cur_client = next;
    }
   rwlock_unlock(&client_list.rwlock, "remove_all_connections");

    lock_mutex(&http_list.mutex, "remove_all_connections");
    http_t *cur_http = http_list.head;
    while(NULL != cur_http){
        http_t *next = cur_http->next;
        pthread_cancel(cur_http->pthread_http);
        cur_http = next;
    }
    unlock_mutex(&http_list.mutex, "remove_all_connections");
}

void client_cancel_handler(void *param){
    client_t *client = (client_t *)param;
    if (client == NULL){
        fprintf(stderr, "client_cancel_handler: param was NULL\n");
        return;
    }
    client_remove(client, &client_list);
    return;
}

void http_cancel_handler(void *param) {
    http_t *http = (http_t *)param;
    if (http == NULL){
        fprintf(stderr, "http_cancel_handler: param was NULL\n");
        return;
    }
    remove_http(http, &http_list, &cache);
    return;
}

int open_socket(int port){
    int sockfd = socket(AF_INET, SOCK_STREAM, 0); //create socket
    if (-1 == sockfd){
        perror("open_socket: socket error");
        return -1;
    }

    struct sockaddr_in server_adr;
    memset(&server_adr, 0, sizeof (struct sockaddr_in));
    server_adr.sin_family = AF_INET; //ipv4 address
    server_adr.sin_addr.s_addr = htonl(INADDR_ANY); //ставим возможность чтения для всех интерфейсов (принимаем любые адресса)
    server_adr.sin_port = htons(port); //listening on this port         //1011 11 13  
    //привязываем адресс к сокету (присваиваем IP к адресу)
    if (-1 == (bind(sockfd, (struct sockaddr *)&server_adr, sizeof (struct sockaddr_in)))){      
        perror("open_socket: bind error");
        close(sockfd);
        return -1;
    }
    //слушаем на сокете. SOMAXCONN - максимальное кол-во подключений в очереди на прослушивания сокета.
    //эта очередь существует для того, чтобы обрабатывать подключение от клиента, пока другие ждут в очереди. 
    //если к серверу попытаются подключиться больше клиентов, чем количество отложенных подключений(SOMAXCONN), эти подключения будут отброшены
    if (-1 == listen(sockfd, SOMAXCONN)){       
        perror("open socket: listen error");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

void *client_thread(void *param){
    
    if (NULL == param){
        return NULL;
    }
    client_t *client = (client_t *) param;
    pthread_detach(client->pthread_client);

    add_client_to_list(client, &client_list); //добавляем в лист клиентов
    printf("[%d] Connected\n", client->sockfd);

    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    pthread_cleanup_push(client_cancel_handler, client);
    int errorCode;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    errorCode = pthread_sigmask(SIG_SETMASK, &set, NULL);
    if(0 != errorCode){
        print_error("main: pthread_sigmask error", errorCode);
        return NULL;
    }
    while(!IS_ERROR_OR_DONE_STATUS(client->status)){   
        while (client->status == AWAITING_REQUEST){
            client_read_data(client, &http_list, &cache);
            if(IS_ERROR_OR_DONE_STATUS(client->status)){    //чекаем состояние клиента: если клиент уже закончил получение данных или сломался, то удаляем его
                return NULL;
            }
        }
        while (client->status == DOWNLOADING || client->status == GETTING_FROM_CACHE){
            if(IS_ERROR_OR_DONE_STATUS(client->status)){    //чекаем состояние клиента: если клиент уже закончил получение данных или сломался, то удаляем его
                return NULL;
            }

            while(1){
                ssize_t size;
                client_update_http_info(client);  //обновляем инфу по клиенту (статус запроса и готовность к получению кэша)
                check_finished_writing_to_client(client); //чекаем закончил ли клиент запись всех данных по запросу
                if(client->status == DOWNLOADING){
                    lock_mutex(&client->http_entry->mutex, "client_thread");
                    if(client->bytes_written == client->http_entry->data_size){
                        cond_wait(&client->http_entry->mutex, &client->http_entry->cond, "client_thread");
                        unlock_mutex(&client->http_entry->mutex, "client_thread");
                    }
                    else{
                        unlock_mutex(&client->http_entry->mutex, "client_thread");
                        break;
                    }
                }
                else break;
            }
            write_to_client(client);
        }
    }
    pthread_cleanup_pop(1); 
    return NULL;
}

void *http_thread(void *param){
    if (NULL == param){
        return NULL;
    }
    http_t *http = (http_t *) param;
    pthread_detach(http->pthread_http);

    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    pthread_cleanup_push(http_cancel_handler, http);

    int errorCode;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    errorCode = pthread_sigmask(SIG_SETMASK, &set, NULL);
    if(0 != errorCode){
        print_error("main: pthread_sigmask error", errorCode);
        return NULL;
    }

    while(http->status == AWAITING_REQUEST){
        http_send_request(http);
        if(http_check_disconnect(http)){ //чекаем пропало ли соединение по данному запросу
            return NULL;
        }
    }
    while (!IS_ERROR_OR_DONE_STATUS(http->status)){
        http_read_data(http, &cache);
        if(http_check_disconnect(http)){ //чекаем пропало ли соединение по данному запросу
            return NULL;
        }
    }
    while(1){
        if(http_check_disconnect(http)){ //чекаем пропало ли соединение по данному запросу
            return NULL;
        }
        lock_mutex(&http->mutex, "http_thread");
        cond_wait(&http->mutex, &http->cond, "http_thread");
        unlock_mutex(&http->mutex, "http_thread");
    }
    pthread_cleanup_pop(1);
    return NULL; 
}

int socks_poll_loop(int server_sockfd){
    int all_accepted_clients = 0;
    int  new_connected_client_fd = 0;

    if (0 != cache_init(&cache)){
        return -1;
    }

    while(!STOPPED_PROGRAMM){
        printf("Listening proxy socket accept new incoming connections...\n");
            
        new_connected_client_fd = accept(server_sockfd, NULL, NULL);
            
        if(-1 == new_connected_client_fd){
            perror("PROXY ACCEPT ERROR: Error while executing a accept\n");
            printf("SERVER HAS STOPPED\n");
            break;
        }
        fprintf(stderr, "new client connected\n");
        create_client(&client_list, new_connected_client_fd);
        all_accepted_clients++;
    }            
    remove_all_connections();
    printf("SERVER HAS STOPPED\n");
}