// Bug: handle_request() frees the body on its error path, and the caller's
// cleanup frees it again.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct request {
  char* body;
  size_t len;
};

int handle_request(struct request* req) {
  if (req->len > 8) {
    free(req->body);  // error path releases the body...
    return -1;
  }
  printf("handled %s\n", req->body);
  return 0;
}

void release_request(struct request* req) {
  free(req->body);  // ...and so does the normal cleanup
  req->body = NULL;
}

int main(void) {
  struct request req = {strdup("payload-too-long"), 16};
  if (handle_request(&req) != 0) fprintf(stderr, "request rejected\n");
  release_request(&req);
  return 0;
}
