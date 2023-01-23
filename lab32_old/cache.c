#include "cache.h"
#include "states.h"
#include <stdio.h>
#include <string.h>

int hash_init(const char *str){
    int length = strlen(str);
    return length % CACHE_SIZE;
}

void free_cache_entry(cache_node_t *entry) {
    if (NULL == entry) 
        return;
    free(entry->host);
    free(entry->path);
    free(entry->data);
    free(entry);
}

void cache_remove(cache_node_t *entry, cache_t *cache) { //уничтожаем данные из кэша
    int hash_index = hash_init(entry->host);  //вычисляем позицию кэша по запросу

    rwlock_wrlock(&cache->rwlocks[hash_index], "cache_remove");

    if (entry == cache->content[hash_index]) {  
        cache->content[hash_index] = entry->next; 
        if (cache->content[hash_index] != NULL){ //если в списке ещё остались элементы
            cache->content[hash_index]->prev = NULL;
        }
    }
    else {
        entry->prev->next = entry->next;
        if (entry->next != NULL) 
            entry->next->prev = entry->prev;
    }

    rwlock_unlock(&cache->rwlocks[hash_index], "cache_remove");
    free_cache_entry(entry);
}

void cache_destroy_single_entry(cache_node_t *cache_node){
    cache_node_t *cur = cache_node;

    while (NULL != cur){
        cache_node_t *next = cur->next;
        free_cache_entry(cur);
        cur = next;
    }
}

int cache_init(cache_t *cache){
    for (size_t i = 0; i < CACHE_SIZE; i++){
        cache->content[i] = NULL;
        if (-1 == rwlock_init(&cache->rwlocks[i], "cache_init")){
            for (size_t j = 0; j < i; j++){
                rwlock_destroy(&cache->rwlocks[j], "cache_init");
            }
            return -1;
        }
    }
    return 0;
}

void cache_destroy(cache_t *cache){
    for (size_t i = 0; i < CACHE_SIZE; i++){
        cache_destroy_single_entry(cache->content[i]);
        cache->content[i] = NULL;
        rwlock_destroy(&cache->rwlocks[i], "cache_destroy");
    }
}

cache_node_t *cache_add(char *host, char *path, char *data, ssize_t size, cache_t *cache) {
    cache_node_t *node = (cache_node_t *)malloc(sizeof(cache_node_t));
    if (NULL == node) {
        perror("cache_add: unable to allocate memory for cache entry");
        return NULL;
    }

    int hash_index = hash_init(host);

    node->is_full = 0;
    node->size = size;
    node->data = data;
    node->host = host;
    node->path = path;

    rwlock_wrlock(&cache->rwlocks[hash_index], "cache_add");

    node->prev = NULL;
    node->next = cache->content[hash_index];  //добавляем в начало списка
    cache->content[hash_index] = node;
    if (NULL != node->next) 
        node->next->prev = node;
    rwlock_unlock(&cache->rwlocks[hash_index], "cache_add");
    return node;
}

cache_node_t *cache_find(const char *host, const char *path, cache_t *cache) {
    int hash_index = hash_init(host);
    
    rwlock_rdlock(&cache->rwlocks[hash_index],"cache_find");

    cache_node_t *cur = cache->content[hash_index];
    while (NULL != cur) {
        if (STR_EQ(host, cur->host) && STR_EQ(path, cur->path)) break;
        cur = cur->next;
    }

    rwlock_unlock(&cache->rwlocks[hash_index], "cache_find");

    return cur;
}