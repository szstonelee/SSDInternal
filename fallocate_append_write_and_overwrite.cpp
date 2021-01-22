#include <stdlib.h>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <chrono>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iostream>
#include <thread>

unsigned char* prep_buf(const size_t aliginment, const size_t size)
{
  unsigned char* aligned_buf = nullptr;
  int ret = posix_memalign(reinterpret_cast<void**>(&aligned_buf), aliginment, size);
  assert(ret == 0);
  for (int i = 0; i < size; ++i)
  {
    aligned_buf[i] = std::rand() % 128;
  }
  return aligned_buf;
}

void real_write(const int fd, const std::size_t file_size, const std::size_t block_size,
                const unsigned char* buf)
{
  lseek(fd, 0, SEEK_SET);
  for (int i = 0; i < file_size/block_size; ++i)
  {
    int num = write(fd, buf, block_size);
    assert(num == block_size);
    int ret_sync = fsync(fd);
    assert(ret_sync == 0);
  }
}

void buffer_append(const std::size_t file_size, const std::size_t block_size,
                   const char* file_path, const unsigned char* buf)
{
  if (access(file_path, F_OK) == 0) 
  {
    unlink(file_path);  // guarantee to start from a brand new file
    sync();
  }

  const auto start = std::chrono::steady_clock::now();

  constexpr mode_t kMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;  // 0666
  const int fd = open(file_path, O_CREAT | O_RDWR, kMode);
  assert(fd != -1);

  real_write(fd, file_size, block_size, buf);

  const auto duration = std::chrono::steady_clock::now() - start;
  std::cout << "new file without fallocate, time(ms) = " 
            << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() << '\n';
  
  int ret = close(fd);
  assert(ret == 0);
}

enum class FallocateMode {kNoFill, kFillZero};

void fallocate_append_mode(const FallocateMode mode,
                           const std::size_t file_size, const std::size_t block_size,
                           const char* file_path, const unsigned char* buf)
{
  if (access(file_path, F_OK) == 0)
  {
    unlink(file_path);  // guarantee to start from a brand new file
    sync();
  }

  const auto start = std::chrono::steady_clock::now();

  constexpr mode_t kMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;  // 0666
  const int fd = open(file_path, O_CREAT | O_RDWR, kMode);
  assert(fd != -1);
  int ret_fallocate = -1;
  switch (mode)
  {
  case FallocateMode::kNoFill:
    ret_fallocate = fallocate(fd, 0, 0, file_size);
    break;
  case FallocateMode::kFillZero:
    ret_fallocate = fallocate(fd, FALLOC_FL_ZERO_RANGE, 0, file_size);
    break;
  default:
    assert("no such FallocateMode");
  }
  assert(ret_fallocate == 0);

  real_write(fd, file_size, block_size, buf);

  const auto duration = std::chrono::steady_clock::now() - start;
  std::cout << "new file with fallocate, fallocate param = " << (mode == FallocateMode::kNoFill ? "NoneZero" : "FillZero") 
            << ", time(ms) = " 
            << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() << '\n';

  int ret = close(fd);
  assert(ret == 0);
}

void overwrite(const bool need_fallocate, 
               const std::size_t file_size, const std::size_t block_size,
               const char* file_path, const unsigned char* buf) 
{
  if (access(file_path, F_OK) != 0)
  {
    std::cout << "error, overwrite can not find the file " << file_path << '\n';
    exit(1);
  } 

  const auto start = std::chrono::steady_clock::now();

  constexpr mode_t kMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;  // 0666
  int fd = open(file_path, O_CREAT | O_RDWR, kMode);
  assert(fd != -1);

  if (need_fallocate)
  {
    int ret = fallocate(fd, FALLOC_FL_ZERO_RANGE, 0, file_size);
    assert(ret == 0);
  }

  real_write(fd, file_size, block_size, buf);

  const auto duration = std::chrono::steady_clock::now() - start;
  std::cout << "overwrite " << (need_fallocate ? "with fallocate" :"without fallocate") 
            << ", time(ms) = " 
            << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() << '\n';

  int ret = close(fd);
  assert(ret == 0);
}

int main()
{
  using namespace std::chrono_literals;

  constexpr std::size_t kBlockSize = 4 << 10;   // 4K
  constexpr std::size_t kFileSize = 500 << 20;  // 500M

  auto buf = std::unique_ptr<unsigned char, void(*)(void*)>(prep_buf(kBlockSize, kBlockSize), free);

  constexpr char file3[] = "/tmp/t3";
  fallocate_append_mode(FallocateMode::kFillZero, kFileSize, kBlockSize, file3, buf.get());
  std::this_thread::sleep_for(500ms);

  constexpr char file2[] = "/tmp/t2";
  fallocate_append_mode(FallocateMode::kNoFill, kFileSize, kBlockSize, file2, buf.get());
  std::this_thread::sleep_for(500ms);

  constexpr char file1[] = "/tmp/t1";
  buffer_append(kFileSize, kBlockSize, file1, buf.get());  
  std::this_thread::sleep_for(500ms);

  overwrite(false, kFileSize, kBlockSize, file1, buf.get()); // use existing file1 for overwrite and no fallocate
  std::this_thread::sleep_for(500ms);

  overwrite(true, kFileSize, kBlockSize, file1, buf.get()); // use existing file1 for overwrite with fallocate
  std::this_thread::sleep_for(500ms);

  return 0;
}