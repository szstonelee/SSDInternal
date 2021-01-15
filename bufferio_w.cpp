// reference some source code from https://github.com/neoremind/io_benchmark/blob/master/bufferio_w.cpp

#include <iostream>
#include <unistd.h>
#include <vector>
#include <thread>
#include <fcntl.h>
#include <chrono>
#include <cassert>
#include <errno.h>
#include <string.h>

#define WRITE_ONCE_BYTE_SIZE 4096
constexpr int kBlockNum = 100000;   // 尽量大点，让data达到几百兆或更高(但同时保证测试机器os page cache占主要内存)，避免SSD内部的优化算法的干扰

static char data[WRITE_ONCE_BYTE_SIZE*kBlockNum] __attribute__((aligned(WRITE_ONCE_BYTE_SIZE))) = {'a'};
constexpr int kConcurrency = 4;
std::chrono::steady_clock::time_point ends[kConcurrency];
static const uint64_t kWriteCountPerThread = 4 / kConcurrency * 1000 * 1000;  // 1 thread 16G, 2 threads 8G each, 4 threads 4G each
static const uint64_t kWriteBytesPerThread = WRITE_ONCE_BYTE_SIZE * kWriteCountPerThread;
static const uint64_t kTotalWriteBytes = kWriteBytesPerThread * kConcurrency;

void writer(const int index) {
  const std::string fname = "/tmp/iotest/data" + std::to_string(index);

  const int fd = ::open(fname.c_str(), O_NOATIME | O_RDWR | O_CREAT, 0644);
  if (fd == -1) {
    std::cout << "open() failed because " << strerror(errno) << '\n';
    exit(1);
  }

  int ret;
  ret = posix_fallocate(fd, 0, kWriteBytesPerThread);
  if (ret != 0) { 
    std::cout << "fallocate err " << ret << '\n';
    exit(1);
  }
  ret = lseek(fd, 0, SEEK_SET);
  if (ret == -1) {
    std::cout << "lseek() failed because " << strerror(errno) << '\n';
    exit(1);
  }

  for (int i = 0; i < kWriteCountPerThread; i++) {
    ret = ::write(fd, data + (i % kBlockNum), WRITE_ONCE_BYTE_SIZE);
    assert(ret == WRITE_ONCE_BYTE_SIZE);
  }

  ret = fdatasync(fd);
  if (ret == -1) {
    std::cout << "fdatasync() failed because " << strerror(errno) << '\n';
    exit(1);
  }

  ret = close(fd);
  if (ret == -1) {
    std::cout << "close() failed because " << strerror(errno) << '\n';
    exit(1);
  }

  ends[index] = std::chrono::steady_clock::now();
}

void init_data_random_char() {
  static const char alphanum[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";

  for (int i = 0; i < sizeof(data); ++i) {
    data[i] = alphanum[rand() % (sizeof(alphanum)-1)];
  }
}

int64_t max_us() {
  std::chrono::steady_clock::duration min_dur = ends[0].time_since_epoch();
  std::chrono::steady_clock::duration max_dur = min_dur;

  for (int i = 1; i < kConcurrency; ++i) {
    std::chrono::steady_clock::duration cur_dur = ends[i].time_since_epoch();
    if (cur_dur < min_dur) {
      min_dur = cur_dur;
    } else if (cur_dur > max_dur) {
      max_dur = cur_dur;
    }
  }
  return std::chrono::duration_cast<std::chrono::microseconds>(max_dur - min_dur).count();
}

int main() {
  srand( (unsigned) time(NULL) * getpid());
  init_data_random_char();

  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  for(int i = 0; i < kConcurrency; i++) {
    std::thread worker(writer, i);
    threads.push_back(std::move(worker));
  }
  for (int i = 0; i < kConcurrency; i++) {
    threads[i].join();
  }  
  const auto duration = std::chrono::steady_clock::now() - start;  
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();

  std::cout << "time elapsed microsecond(us) " << us << ", " << kTotalWriteBytes / us << " MB/s\n";
  std::cout << "elapse time(us) between max thread and min thread " << max_us() << '\n';
  return 0;
}
