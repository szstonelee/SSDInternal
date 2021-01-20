#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <chrono>
#include <cassert>
#include <iostream>

void clear_page_cache()
{
  sync();
  int fd = -1;
  fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
  if(fd == -1)
  {
    std::cout << "clear page cache needs administrator privilege, please sudo to run the command\n";
    exit(1);
  }
  int num = 0;
  num = write(fd, "1", 1);
  assert(num == 1);
  int ret = -1;
  ret = close(fd);  
  assert(ret == 0);

  fd = open("/sys/block/vda/queue/read_ahead_kb", O_WRONLY);
  if (fd == -1)
  {
    std::cout << "maybe block device is not correct, check it and modify the source code\n";
    exit(1);
  }
  num = write(fd, "0", 1);
  assert(num == 1);
  ret = close(fd);
  assert(ret == 0);
}

void write_file(const int fd, const size_t block_size, const char ch)
{
  constexpr size_t kBufTotal = 4096;
  assert(block_size <= kBufTotal);
  assert(ch != 'x');
  lseek(fd, 0, SEEK_SET);
  unsigned char* buf;
  int ret = -1;
  ret = posix_memalign(reinterpret_cast<void**>(&buf), block_size, kBufTotal);
  assert(ret == 0);
  for (int i = 0; i < kBufTotal; ++i)
  {
    if (i < block_size)
    {
      buf[i] = ch;
    }
    else
    {
      buf[i] = 'x';
    }
  }
  int num = pwrite(fd, buf, block_size, 0);
  assert(num == block_size);
  ret = fsync(fd);
  assert(ret == 0);
  free(buf);
}

int main()
{
  std::cout << "PID = " << getpid() << '\n';

  constexpr mode_t kMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
  const char kFilePath[] = "/tmp/broken";

  int ret = -1;
  int fd = -1;

/*
  if (access(kFilePath, F_OK) == 0)
  {
    ret = unlink(kFilePath);
    assert(ret == 0);
  }

  fd = open(kFilePath, O_RDWR | O_CREAT, kMode);
  assert(fd != -1);
  write_file(fd, 4096, 'a');
  ret = close(fd);
  assert(ret == 0);
*/

  clear_page_cache();

  fd = open(kFilePath, O_RDWR | O_CREAT | O_DIRECT, kMode);
  assert(fd != -1);
  write_file(fd, 512, 'b');
  ret = close(fd);
  assert(ret == 0);

  return 0;
}