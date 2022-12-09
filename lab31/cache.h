#ifndef LAB31_CACHE_H

#include <stdlib.h>

#define LAB31_CACHE_H

#define CACHE_SIZE 64

typedef struct cache_node{
    int is_full;
    char *data;
    ssize_t size;
    char *host, *path;
    struct cache_node *next, *prev;
} cache_node_t;

typedef struct cache{
    cache_node_t *content[CACHE_SIZE];
} cache_t;

int cache_init(cache_t *cache);
void cache_destroy(cache_t *cache);
cache_node_t *cache_find(const char *host, const char *path, cache_t *cache);
cache_node_t *cache_add(char *host, char *path, char *data, ssize_t size, cache_t *cache);
void cache_remove(cache_node_t *entry, cache_t *cache);

#endif //LAB31_CACHE_H
