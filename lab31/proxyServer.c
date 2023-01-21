#include "proxyServer.h"
#include "client.h"

client_list_t client_list = {.head = NULL, .length = 0};
http_list_t http_list = {.head = NULL, .length = 0};
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
    client_t *cur_client = client_list.head;
    while (NULL != cur_client){
        client_t *next = cur_client->next;
        client_remove(cur_client, &client_list);
        cur_client = next;
    }
    
    http_t *cur_http = http_list.head;
    while(NULL != cur_http){
        http_t *next = cur_http->next;
        remove_http(cur_http, &http_list, &cache);
        cur_http = next;
    }
}

int open_socket(int port){
    int sockfd = socket(AF_INET, SOCK_STREAM, 0); 
    if (-1 == sockfd){
        perror("open_socket: socket error");
        return -1;
    }

    struct sockaddr_in server_adr;
    memset(&server_adr, 0, sizeof (struct sockaddr_in));
    server_adr.sin_family = AF_INET; //ipv4
    server_adr.sin_addr.s_addr = htonl(INADDR_ANY); //ставим возможность чтения для всех интерфейсов (принимаем любые адресса)
    server_adr.sin_port = htons(port); //слушаем на этом порте        //1011 11 13  
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

int socks_poll_loop(int server_sockfd){

    if (0 != cache_init(&cache)){
        return -1;
    }

    int SERVERFD_SIZE = FD_SETSIZE; //максимальное кол-во возможных подключенных клиентов и их соединений + деск сервера
    struct pollfd serverFd[SERVERFD_SIZE]; 
    
    int new_connected_client_fd = 0;  //дескриптор нового клиента

    //каждую итерацию цикла: 0 - прокси, последующие элементы клиенты и запросы
    
    for (size_t i = 0; i < SERVERFD_SIZE; i++){
        serverFd[i].fd = -1;
    }

    serverFd[0].fd = server_sockfd;
    serverFd[0].events = POLLIN;  //ставим на чтение(прием новых клиентов)
    
    while(!STOPPED_PROGRAMM){
        int poll_status;
        errno = 0;
        poll_status = poll(serverFd, SERVERFD_SIZE, -1);
        //printf("%d\n", poll_status);
        
        if (-1 == poll_status && errno != EINTR){
            print_error("proxyServer error: poll()\n", errno);
            printf("SERVER HAS STOPPED\n");
            break;
        }

        if (0 == poll_status){
            continue;
        }
        //смотрим на ошибки у сервера
        //если принятый сокет недоступен       //POLLERR - ошибки в дискприторе      //POLLHUP - "бросили трубку"        //POLLNVAL - файловый дескриптор не открыт
        if(serverFd[0].revents & POLLERR || serverFd[0].revents & POLLHUP || serverFd[0].revents & POLLNVAL){
            printf("proxyServer error: poll(): accept socket is unavailable\n");
            printf("SERVER HAS STOPPED\n");
            break; 
        }

        //если есть новое подключение
        if (serverFd[0].revents & POLLIN){
            printf("Listening proxy socket accept new incoming connections...\n");
            //poll чекает заблокируется ли акспепт или нет
            //если не заблокируется то вернетeся управление poll (в ревентс POLLIN)

            new_connected_client_fd = accept(server_sockfd, NULL, NULL);
            if(-1 == new_connected_client_fd){
                perror("PROXY ACCEPT ERROR: Error while executing a accept\n");
                printf("SERVER HAS STOPPED\n");
                break;
            }
            fprintf(stderr, "new client connected\n");
            create_client(&client_list, new_connected_client_fd);          
        }

        serverFd[0].fd = client_list.length >= MAX_NUMBER_OF_CLIENTES ? -1 : server_sockfd; //если кол во подсоединенных клиентов больше назначенного, то мы ставим -1
                                                                                              // чтобы poll игнорировал новые подключения
        serverFd[0].events = POLLIN;  //ставим на чтение(прием новых клиентов)
        serverFd[0].revents = 0;

        //чекаем если что нибудь поменялось
        {
            client_t *cur_client = client_list.head;
            while(NULL != cur_client){
                int index = cur_client->sockfd;
                client_t *next = cur_client->next;
                if (!cur_client->just_created){
                    if((serverFd[index].revents & POLLIN || serverFd[index].revents & POLLHUP) && !IS_ERROR_OR_DONE_STATUS(cur_client->status)){ //если клиент ещё не закончил подключение
                        client_read_data(cur_client, &http_list, &cache); //начинаем считывать данные
                    }

                    if (serverFd[index].revents & POLLOUT && ((cur_client->status == DOWNLOADING && cur_client->http_entry->status == DOWNLOADING && cur_client->bytes_written < cur_client->http_entry->data_size) ||
                        (cur_client->status == GETTING_FROM_CACHE && cur_client->bytes_written < cur_client->cache_node->size))){
                        write_to_client(cur_client, client_list.length);
                    }
                }

                cur_client->just_created = 0;

                if(IS_ERROR_OR_DONE_STATUS(cur_client->status)){    //чекаем состояние клиента: если клиент уже закончил получение данных или сломался, то удаляем его
                    serverFd[index].fd = -1;
                    client_remove(cur_client, &client_list); //удаляем его
                    cur_client = next; //переходим к сл клиенту
                    continue;
                }

                client_update_http_info(cur_client);  //обновляем инфу по клиенту (статус запроса и готовность к получению кэша)
                check_finished_writing_to_client(cur_client); //чекаем закончил ли клиент запись всех данных по запросу
                serverFd[index].fd = cur_client->sockfd;    //вставляем декскриптор клиента для отслеживания
                serverFd[index].events = 0; //ставим на чтение запроса
                serverFd[index].revents = 0;

                if(cur_client->status == AWAITING_REQUEST){
                    serverFd[index].events = POLLIN;
                }
                time_t dif = time(0) - cur_client->last_send_time;
                
                if (dif >= 1){
                    cur_client->cur_allowed_size = MAX_SEND_SIZE / client_list.length;
                }
                        //если клиент ещё получает данные из кэша или только скачивает их
                if (cur_client->cur_allowed_size > 0 && ((cur_client->status == DOWNLOADING && cur_client->bytes_written < cur_client->http_entry->data_size) ||
                            (cur_client->status == GETTING_FROM_CACHE && cur_client->bytes_written < cur_client->cache_node->size))) {
                    serverFd[index].events |= POLLOUT; //ждем записи
                }

                cur_client = next; //переходим к сл клиенту
            }

            http_t *cur_http = http_list.head;
            while (NULL != cur_http){
                int index = cur_http->sockfd_copy;
                http_t *next = cur_http->next;
                if (!cur_http->just_created){
                    if ((serverFd[index].revents & POLLIN || serverFd[index].revents & POLLHUP) && !IS_ERROR_OR_DONE_STATUS(cur_http->status)){
                        http_read_data(cur_http, &cache, http_list.length);
                    }
                    if (serverFd[index].revents & POLLOUT && cur_http->status == AWAITING_REQUEST){
                        http_send_request(cur_http);
                    }
                }

                cur_http->just_created = 0;
                
                if(http_check_disconnect(cur_http)){ //чекаем пропало ли соединение по данному запросу
                    serverFd[index].fd = -1;
                    remove_http(cur_http, &http_list, &cache); //уничтожаем соединение и кэш который мы записали
                    cur_http = next;
                    continue;
                }

                serverFd[index].events = 0; //очищаем от мусора
                serverFd[index].revents = 0;
                serverFd[index].fd = cur_http->sockfd;

                time_t dif = time(0) - cur_http->last_recv_time;
                if (dif >= 1){
                    cur_http->cur_allowed_size = MAX_SEND_SIZE / http_list.length;
                }

                if (cur_http->cur_allowed_size > 0 && cur_http->status == DOWNLOADING){  
                    serverFd[index].events |= POLLIN; //ждем чтение
                }
                
                if(cur_http->status == AWAITING_REQUEST){  //если запрос в ожидании
                    serverFd[index].events |= POLLOUT;  //ждем запись
                }

                cur_http = next;
            }
            
            
        }
    }
    remove_all_connections();
    printf("SERVER HAS STOPPED\n");
}