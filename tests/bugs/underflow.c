// Bug: bucket_of() returns -1 for negative samples, and the histogram is
// indexed with it, writing one int before the start of the block.
#include <stdio.h>
#include <stdlib.h>

enum { kBuckets = 8 };

int bucket_of(int rssi_dbm) {
  if (rssi_dbm < 0) return -1;  // meant to clamp to bucket 0
  return rssi_dbm / 16 < kBuckets ? rssi_dbm / 16 : kBuckets - 1;
}

int* build_histogram(const int* samples, int n) {
  int* hist = calloc(kBuckets, sizeof(int));
  for (int i = 0; i < n; ++i) hist[bucket_of(samples[i])]++;
  return hist;
}

int main(void) {
  const int samples[] = {12, 40, -3, 77, 100};
  int* hist = build_histogram(samples, 5);
  for (int b = 0; b < kBuckets; ++b) printf("%d ", hist[b]);
  printf("\n");
  free(hist);
  return 0;
}
