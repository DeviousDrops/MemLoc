// Bug: the loop runs to i <= n, writing one element past the end of an array
// allocated with new[].
#include <cstdio>

long* make_squares(int n) {
  long* squares = new long[n];
  for (int i = 0; i <= n; ++i) squares[i] = static_cast<long>(i) * i;  // i < n
  return squares;
}

int main() {
  long* squares = make_squares(10);
  std::printf("%ld\n", squares[9]);
  delete[] squares;
  return 0;
}
