// Bug: skip_spaces() returns a pointer into the middle of the line, and that
// pointer (not the one malloc returned) is freed.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* skip_spaces(char* s) {
  while (isspace((unsigned char)*s)) ++s;
  return s;
}

void process_line(const char* text) {
  char* line = strdup(text);
  char* word = skip_spaces(line);
  printf("command: %s\n", word);
  free(word);  // should be free(line)
}

int main(void) {
  process_line("        connect 00:1a:7d:da:71:13");
  return 0;
}
