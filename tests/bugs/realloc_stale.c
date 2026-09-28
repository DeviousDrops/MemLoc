// Bug: append() may move the buffer with realloc, but the caller keeps using a
// pointer into the old block.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct buffer {
  char* data;
  size_t len, cap;
};

void append(struct buffer* b, const char* s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2;
    b->data = realloc(b->data, b->cap);
  }
  memcpy(b->data + b->len, s, n + 1);
  b->len += n;
}

int main(void) {
  struct buffer b = {NULL, 0, 0};
  append(&b, "hdr:");
  char* header = b.data;  // points into the current block
  append(&b, "a much longer payload that forces the buffer to grow");
  header[0] = 'H';  // stale: the block moved
  printf("%s\n", b.data);
  free(b.data);
  return 0;
}
