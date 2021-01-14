# 前言

[在网上看到一个程序员的作品](https://zhuanlan.zhihu.com/p/178670421)

在[其github](https://github.com/neoremind/io_benchmark)里有用C++写了一个程序测试SSD的throughput，觉得很好，拿来测试一下。

# 我的微调

参考：[bufferio_w.cpp](bufferio_w.cpp)

1. 原来每个线程是顺序写入一个文件，data_index，4G大小一个。每次写4K，都是字符a

为了防止写入数据熵一样，我修改了一下，加了一个init_data_random_char()函数，让data[]里的字符随机，这样避免SSD内部压缩算法的影响。

2. 同时，在close()前，增加了fdatasync()，避免memory最后的影响。比如：测试机器几十个G内存，那么写4G的文件，可能全部落在内存里，没有落盘。

3. 其次，如果是一个线程，写入16G；2个线程，每个线程8G，以此类推。写入文件总量相对于我的内存(3G)保证要大很多。

4. 最后，有一个算法的担心。当多线程时，如果所有线程都差不多是同时结束，那么throughput的计算是准确的，如果不是，那么可能有比较大的误差。我这里只能假设每个线程结束时间差不多，同时在程序加入了max_us()打印最后退出线程和最先退出线程之间的时间差。

编译
```
g++ -std=c++17 bufferio_w.cpp -pthread -O2
```

运行
```
sudo mkdir -p \tmp\iotest
sudo ./a.out
```

# 我的Mac上的SSD测试数据

修改源代码里的kConcurrency参数，得到不同thread数目下的write buffered sequential throughtput

测试五次(每次测试前都删除目录中的data文件，rm data*)，然后下表列出每次测试的值

| thread count | No. of test | throughtput | total time(us) | thread elapse time(us) |
| -- | -- | -- | -- | -- |
| 1 | 1 | 178 MB/s | 91790488 | 0 |
|   | 2 | 176 MB/s | 92874571 | 0 |
|   | 3 | 172 MB/s | 94763742 | 0 |
|   | 4 | 194 MB/s | 84322618 | 0 |
|   | 5 | 190 MB/s | 85953153 | 0 |
| 2 | 1 | 167 MB/s | 97819224 | 383 |
|   | 2 | 165 MB/s | 99130729 | 20916 |
|   | 3 | 170 MB/s | 96321469 | 398126 |
|   | 4 | 167 MB/s | 98028987 | 11123 |
|   | 5 | 165 MB/s | 99076310 | 630695 |
| 4 | 1 | 169 MB/s | 96564133 | 1409336 |
|   | 2 | 156 MB/s | 104973877 | 1476049 |
|   | 3 | 158 MB/s | 103368254 | 25361 |
|   | 4 | 150 MB/s | 108930002 | 1472597 |
|   | 5 | 179 MB/s | 91300013 | 1262070 |

# 分析

[可以对比一下之前我用fio测试的数据](scenario.md)

1. 和fio相比，throughput要略弱，但一个数量级。

2. 线程数增加，影响不大，还稍少。

3. 线程数增多，thread elapse所产生的误差加大，不过基本可控。对于2线程，elapse占total的1%不到；4线程不到2%。