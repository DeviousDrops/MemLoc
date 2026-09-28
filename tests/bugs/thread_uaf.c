// Bug: the worker thread frees a job once it has processed it, but the
// producer writes the job's status afterwards.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

struct job {
  int id;
  int status;
  char payload[48];
};

void* worker_main(void* arg) {
  struct job* job = arg;
  printf("processing job %d\n", job->id);
  free(job);  // the worker owns the job after hand-off...
  return NULL;
}

void submit_job(int id) {
  struct job* job = calloc(1, sizeof(*job));
  job->id = id;
  pthread_t worker;
  pthread_create(&worker, NULL, worker_main, job);
  pthread_join(worker, NULL);
  job->status = 1;  // ...but the producer still marks it done
}

int main(void) {
  submit_job(42);
  return 0;
}
