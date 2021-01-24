// copy from https://zhuanlan.zhihu.com/p/61212603, code copyright belong to the author, Chen,Zhongzhi

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>
#include <random>

uint64_t NowMicros() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

int main()
{
  uint64_t st, ed;
  off_t file_size = 1 * 1024 * 1024 * 1024;
  int fd = open("/tmp/tf", O_CREAT | O_RDWR, 0666);
  st = NowMicros();
  // int ret;
  int ret = fallocate(fd, 0, 0, file_size);
  if (ret != 0) { 
    printf("fallocate err %d\n", ret);
  }
  ed = NowMicros();
  printf("fallocate time microsecond(us) %lld\n", ed - st);
  lseek(fd, 0, SEEK_SET);
  int dsize = 4096;
  unsigned char *aligned_buf;
  ret = posix_memalign((void **)&aligned_buf, 4096, 4096 * 10);
  for (int i = 0; i < dsize; i++) {
    aligned_buf[i] = (int)random() % 128;
  }
  st = NowMicros();
  int num;
  for (uint64_t i = 0; i < file_size / dsize; i++) {
    num = write(fd, aligned_buf, dsize);
    fsync(fd);
    if (num != dsize) {
      printf("write error\n");
      return -1;
    }
  }
  ed = NowMicros();
  printf("first write time microsecond(us) %lld\n", ed - st);

  sleep(10);
  lseek(fd, 0, SEEK_SET);
  st = NowMicros();
  for (uint64_t i = 0; i < file_size / dsize; i++) {
    num = write(fd, aligned_buf, dsize);
    fsync(fd);
    if (num != dsize) {
      printf("write error\n");
      return -1;
    }
  }
  ed = NowMicros();
  printf("second write time microsecond(us) %lld\n", ed - st);
  return 0;
}