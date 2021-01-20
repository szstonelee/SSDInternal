// this is a copy of some code from https://zhuanlan.zhihu.com/p/61002228
// because I need to test the results as described in the article 

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>
#include <random>

#include <linux/falloc.h>
#include <sys/syscall.h>

uint64_t NowMicros() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

int main()
{
  uint64_t st, ed;
  uint64_t file_size = 10LL * 1024LL * 1024LL;
  int fd = open("/tmp/tf_test", O_CREAT | O_RDWR, 0666);
  int ret;
  unsigned char *aligned_buf;
  int dsize = 512;
  ret = posix_memalign((void **)&aligned_buf, 4096, 4096 * 10);
  for (int i = 0; i < dsize; i++) {
    aligned_buf[i] = (int)random() % 128;
  }

  lseek(fd, 0, SEEK_SET);
  st = NowMicros();
  int num;
  off_t off = 0;
  for (uint64_t i = 0; i < file_size / dsize; i++) {
    num = pwrite(fd, aligned_buf, dsize, off);
    off += 512;
    fsync(fd);
    if (num != dsize) {
      printf("write error num %d\n", num);
      return -1;
    }
  }
  ed = NowMicros();
  printf("write time microsecond(us) %lld\n", (long long)(ed - st));

  return 0;
}