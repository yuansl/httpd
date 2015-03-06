#ifndef MAP_H
#define MAP_H
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>

struct m_string {
	size_t size;
	char *data;
};

struct map {
	struct m_string first;
	struct m_string second;
	struct map *left, *right;
};

bool s_empty(struct m_string *s);
size_t s_size(struct m_string *s);

const char *map_at(struct map *map, const char *key);
void map_insert(struct map *map_head, const char *key, const char *val);
void map_destructor(struct map *map);

#endif
