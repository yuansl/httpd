#include "map.h"

size_t s_size(struct m_string *s)
{
	return s->size;
}

bool s_empty(struct m_string *s)
{
	return s->size == 0;
}

static struct map *pmalloc(size_t size, const char *key, const char *val)
{
	struct map *ret = malloc(size);

	assert(ret != NULL);

	ret->first.size = strlen(key) + 1;
	ret->first.data = malloc(ret->first.size);

	ret->second.size = strlen(val) + 1;
	ret->second.data = malloc(ret->second.size);
	
	strcpy(ret->first.data, key);	
	strcpy(ret->second.data, val);
	ret->left = ret->right = NULL;

	return ret;
}

void map_insert(struct map *map_head, const char *key, const char *val)
{
	struct map *x, *prev, *cur;
	int sign;

	prev = NULL;
	cur = map_head;
	sign = 0;

	while (cur && (sign = strcmp(key, cur->first.data)) != 0) {
		prev = cur;
		if (sign < 0)
			cur = cur->left;
		else
			cur = cur->right;
	}
	
	if (prev == NULL && cur == NULL) {
		fprintf(stderr, "map_head is empty\n");
		return;
	}
	
	if (cur == NULL) {
		x = pmalloc(sizeof(struct map), key, val);
		if (sign < 0)
			prev->left = x;
		else
			prev->right = x;
		return;
	}
	free(cur->second.data);
	cur->second.size = strlen(val) + 1;
	cur->second.data = malloc(cur->second.size);
	strcpy(cur->second.data, val);
}

const char *map_at(struct map *map, const char *key)
{
	struct map *cur;
	int sign;
	
	cur = map;
	while (cur && (sign = strcmp(key, cur->first.data)) != 0) {
		if (sign < 0)
			cur = cur->left;
		else
			cur = cur->right;
	}
	if (cur == NULL)
		return NULL;
	return cur->second.data;
}

void map_destructor(struct map *map)
{
	if (map) {
		if (map->left)
			map_destructor(map->left);
		if (map->right)
			map_destructor(map->right);
		free(map);
	}
}
