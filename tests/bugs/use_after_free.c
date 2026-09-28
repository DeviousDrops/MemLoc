// Bug: evict() frees a cache entry, but the caller still holds the pointer and
// bumps its hit counter afterwards.
#include <stdio.h>
#include <stdlib.h>

struct entry {
  int key;
  int hits;
  struct entry* next;
};

struct entry* g_cache = NULL;

struct entry* insert(int key) {
  struct entry* e = calloc(1, sizeof(*e));
  e->key = key;
  e->next = g_cache;
  g_cache = e;
  return e;
}

void evict(int key) {
  for (struct entry** pp = &g_cache; *pp != NULL; pp = &(*pp)->next) {
    if ((*pp)->key == key) {
      struct entry* dead = *pp;
      *pp = dead->next;
      free(dead);
      return;
    }
  }
}

void record_hit(struct entry* e) { e->hits++; }

int main(void) {
  insert(1);
  struct entry* hot = insert(2);
  evict(2);
  record_hit(hot);  // hot was freed by evict()
  evict(1);
  printf("done\n");
  return 0;
}
