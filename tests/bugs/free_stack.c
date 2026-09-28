// Bug: the name is heap-allocated only when a user name is given; otherwise
// it points at a local buffer, which is then freed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void greet(const char* user) {
  char fallback[32] = "guest";
  char* name = user != NULL ? strdup(user) : fallback;
  printf("hello, %s\n", name);
  free(name);  // wrong when name == fallback
}

int main(void) {
  greet("admin");
  greet(NULL);
  return 0;
}
