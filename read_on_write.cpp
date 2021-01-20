// referecne from https://zhuanlan.zhihu.com/p/61002228

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <chrono>
#include <cassert>
#include <iostream>

void exit_with_help(const char* command_name)
{
  std::cout << "usage: " << command_name 
            << " 1 or 2 or 3, 1 -- buffered with read, 2 -- buffered without reead, 3 -- no buffered\n";
  exit(1);
}

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

void check_file_before_write(const char* file_path, const int file_size)
{
  if (access(file_path, F_OK) == 0)
    return;  // if exist return anyway
  
  // file not exist, we need to create a file with size of file_size and write it to the file_size
  // NOTE: it does not work if only fseek() to the end and fwrite() one char to the end 
  //       to make the file size grow to the file_size. We need the real write for the whole file.
  // 
  // The coding is like the following
  // 
  // ret = fseek(fp, file_size - 1, SEEK_SET);
  // assert(ret == 0);
  // ret = fwrite("", 1, sizeof(char), fp);
  // assert(ret == 1);
  // 
  assert(file_size > 0);
  FILE* fp = fopen(file_path, "w+");
  if (fp == nullptr)
  {
    std::cout << "failed to create the file " << file_path << ", because " << strerror(errno) << '\n';
    exit(1);
  }
  
  int ret;
  constexpr size_t kBlockSize = 1 << 20;
  unsigned char* buf;
  ret = posix_memalign(reinterpret_cast<void**>(&buf), kBlockSize, kBlockSize); 
  assert(ret == 0); 

  ret = fseek(fp, 0, SEEK_SET);
  assert(ret == 0);
  for (int i = 0; i < file_size/kBlockSize; ++i)
  {
    ret = fwrite(buf, kBlockSize, 1, fp);
    assert(ret == 1);
  }
  
  ret = fclose(fp);
  assert(ret == 0);

  free(buf);
}

int main(int argc, char* argv[])
{
  enum class ActionType {kBufferRead = 1, kBufferNoRead = 2, kNoBuffer = 3,};

  if (argc != 2) 
    exit_with_help(argv[0]);
  int action_integer = std::atoi(argv[1]);
  if (action_integer == 0 
      || action_integer < static_cast<int>(ActionType::kBufferRead) 
      || action_integer > static_cast<int>(ActionType::kNoBuffer))
    exit_with_help(argv[0]);
  ActionType action = static_cast<ActionType>(action_integer);

  constexpr int kFileSize = 500 << 20; // 500M
  constexpr int kPage = 4096;
  constexpr int kSector = 512;
  constexpr int kSectorsPerPage = kPage/kSector;

  unsigned char* aligned_buf;
  int ret = posix_memalign(reinterpret_cast<void**>(&aligned_buf), kPage, kPage * 10);
  assert(ret == 0);
  for (int i = 0; i < kSector; i++) 
  {
    aligned_buf[i] = static_cast<int>(random()) % 128;
  }

  const char kFilePath[] = "/tmp/tf";
  check_file_before_write(kFilePath, kFileSize);

  clear_page_cache();

  int fd = -1;  
  constexpr mode_t kMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;  // 0666
  switch (action) 
  {
  case ActionType::kBufferNoRead:
  case ActionType::kBufferRead:
    fd = open(kFilePath, O_RDWR, kMode);
    break;
  case ActionType::kNoBuffer:
    fd = open(kFilePath, O_RDWR | O_DIRECT | O_SYNC, kMode);
    break;
  default:
    assert("error no such action type!");
  }
  if (fd == -1)
  {
    std::cout << "error open file becuase " << strerror(fd) << '\n';
    exit(2);
  }
  lseek(fd, 0, SEEK_SET);

  const auto start = std::chrono::steady_clock::now();
  off_t off = 0;
  for (int i = 0; i < kFileSize / kSector; i++) 
  {
    switch (action)
    {
    case ActionType::kBufferRead:
    case ActionType::kNoBuffer:
    {
      int num = pwrite(fd, aligned_buf, kSector, off);
      assert(num == kSector);
      break;
    }

    case ActionType::kBufferNoRead:
    // case ActionType::kNoBuffer:
      if (i % kSectorsPerPage == 0)
      {
        memset(aligned_buf+kSector, 0, kPage);
        int num = pwrite(fd, aligned_buf, kPage, off);
        assert(num == kPage);
      }
      else
      {
        int num = pwrite(fd, aligned_buf, kSector, off);
        assert(num == kSector);
      }
      break;
      
    default:
      assert("no such ActionType");
    }

    off += kSector;
    ret = fsync(fd);
    assert(ret == 0);
  }
  const auto duration = std::chrono::steady_clock::now() - start;
  std::string action_name = "";
  action_name = action == ActionType::kBufferNoRead ? "Buffer no read" 
                : (action == ActionType::kBufferRead ? "Buffer read" : "No buffer");
  std::cout << action_name << ", duration(ms) = " 
            << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() << '\n';

  free(aligned_buf);

  return 0;
}

