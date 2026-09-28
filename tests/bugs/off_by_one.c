// Bug: the copy forgets room for the terminating NUL, so strcpy writes one
// byte past the end of the block.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* copy_name(const char* name) {
  char* copy = malloc(strlen(name));  // should be strlen(name) + 1
  strcpy(copy, name);
  return copy;
}

int main(void) {
  char* name = copy_name("wireless-headset");
  printf("device: %s\n", name);
  free(name);
  return 0;
}
