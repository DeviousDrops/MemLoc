// Bug: two allocation sites whose blocks are never freed: every call to
// load_setting() leaks its copy, and make_cache() leaks the whole table.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* load_setting(const char* key) {
  char* value = malloc(64);
  snprintf(value, 64, "%s=enabled", key);
  return value;
}

int* make_cache(int n) {
  int* table = calloc((size_t)n, sizeof(int));
  for (int i = 0; i < n; ++i) table[i] = i * i;
  return table;
}

int main(void) {
  const char* keys[] = {"bluetooth", "wifi", "airdrop"};
  for (int i = 0; i < 3; ++i) printf("%s\n", load_setting(keys[i]));  // never freed
  int* cache = make_cache(256);
  printf("cache[10] = %d\n", cache[10]);  // never freed
  char* ok = load_setting("freed");
  free(ok);
  return 0;
}
