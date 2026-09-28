// Bug: find_device() returns NULL for an unknown id and the caller
// dereferences the result without checking.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct device {
  int id;
  char name[32];
};

struct device* g_devices;
int g_count;

struct device* find_device(int id) {
  for (int i = 0; i < g_count; ++i)
    if (g_devices[i].id == id) return &g_devices[i];
  return NULL;
}

void print_device(int id) {
  struct device* d = find_device(id);
  printf("%d: %s\n", d->id, d->name);  // d is NULL for unknown ids
}

int main(void) {
  g_count = 2;
  g_devices = calloc((size_t)g_count, sizeof(struct device));
  g_devices[0].id = 1;
  strcpy(g_devices[0].name, "keyboard");
  g_devices[1].id = 2;
  strcpy(g_devices[1].name, "trackpad");
  print_device(1);
  print_device(7);
  free(g_devices);
  return 0;
}
