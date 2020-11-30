
# 测试环境

在Mac上的虚拟Linux(基于MultiPass)

## Linux版本

```
cat /proc/version
cat /etc/*-release
```

输出结果是
```
Linux version 5.4.0-48-generic
DISTRIB_ID=Ubuntu
DISTRIB_RELEASE=20.04
```

## OS IO

### IO Scheduler

检查OS IO Scheduler
```
cat /sys/block/<your block device name>/queue/scheduler
```

输出是
```
none [mq-deadline]
```

我们需要将之改为none模式的IO Scheduler。因为是虚拟机，所以没有用比较适合SSD的none模式
```
sudo -i
echo none > /sys/block/<your block device name>/queue/scheduler
cat /sys/block/<your block device name>/queue/scheduler
```

### rotational

确定Linux操作系统不是按旋转磁盘（rotationional）模式对待磁盘
```
cat /sys/block/<your block device name>/queue/rotational
```

结果是1。这不对，需要修改为0
```
sudo -i
echo 0 > /sys/block/<your block device name>/queue/rotational
cat /sys/block/<your block device name>/queue/rotational
```

### read ahead

```
blockdev --getra /dev/<your block device name>

or

cat /sys/block/<your block device name>/queue/read_ahead_kb
```

我们的结果是256，即kernel可以自己根据需要预读128K。为了测试的方便，我们将它设置为0

```
blockdev --setra 0 /dev/<your block device name>
blockdev --getra /dev/<your block device name>

or

echo 0 > /sys/block/<your block device name>/queue/read_ahead_kb
cat /sys/block/<your block device name>/queue/read_ahead_kb
```

## File System

```
df -Th
```

结果是
```
Filesystem         Type        Size  Used Avail Use% Mounted on
/dev/vda1          ext4        9.6G  6.7G  2.9G  71% /
```

## 创建一个测试文件

根基我的磁盘的容量，设置一个2G大小的文件
```
dd if=/dev/zero of=tfile bs=1M count=2048
```

但这个文件有一个问题，就是内容都是0，这不利于测试真实的情况，因为SSD是根据文件内容的熵，散列到内部的cell里进行并发处理。

所以，用真实的数据文件，或者下载一个大小相当的文件包（最好是压缩的文件，比较接近真实的情况）

我是下载了一个Ubuntu 20的Desktop安装版本，有近3G大小。

同时发现，如果文件过小（比如百兆大小文件针对G量级文件），有1倍的数据差异。所以，建议部署接近Production的数据量，比如针对Rocksdb，部署多个文件，总量可达TB级别，参考fio的Target file/device的相关说明。

## read & direct & invalidate

对于read，如果direct=1，那么page cache将不启作用。如果direct=0，但invalidate=1，那么page cache的作用是零

### 清page cache

```
sudo -i
sync; echo 1 > /proc/sys/vm/drop_caches
```

### 查看page cache

```
cat /proc/meminfo | grep Cached
```

然后看```Cached:```这一项，如果只有很少的数量，比如几十M，那么就说明page cache已经清楚干净了

### page cache的热身

```
fio --name=test --filename=tfile --rw=read --ioengine=sync --bs=4k --direct=0
```

然后用上面的查看page cache的命令查看，可以看到```Cached:```到了几个G，说明文件已经被缓存到page cache里了

### read, direct, invalidate的对比

如果read不经过page cache，i.e., direct=1，

```
fio --name=test --filename=tfile --rw=randread --io_size=200M --ioengine=sync --bs=4k --direct=1
```
在我的机器上，throughtput=8MB/s

如果设置direct=0，经过page cache，你会发现结果差不多
```
fio --name=test --filename=tfile --rw=randread --io_size=200M --ioengine=sync --bs=4k --direct=0
```
这是因为还有一个参数invalidate，这个参数是每次读前，将对应的cache先清除，所以，就和direct=1一样了

如果我们增加设置invaliddate，如下
```
fio --name=test --filename=tfile --rw=randread --io_size=200M --ioengine=sync --bs=4k --direct=0 --invalidate=0
```
就会发现throughput=2GB/s左右。这是因为基本都是从内存读到数据（我们之前有做热身）
如果将随机读改为顺序读（同时不启用page cache），命令如下
```
fio --name=test --filename=tfile --rw=read --io_size=200M --ioengine=sync --bs=4k --direct=1
```
这时throughput=17MB/s，说明顺序读是随机读一倍。

# read各种block size下的对比

## 测试注意

如果我们尝试将bs改为其他值，包括8k, 16k, 32k，64k，然后比较随机读和顺序读，我们得到下表

NOTE: 
1. 测试文件大些，如果测试文件过小，或者用了比较小的size值，会导致throughput明显过高（可达1倍）
2. 我们的测试时间需要足够长，至少5分钟以上，否则，请修改io_size参数。如果时间过短，SSD内部的缓存可能起很大作用，带宽有2倍的差别。
3. 建议多测几次，取中间值或概率较多的值。即使每次几分钟，每次的值都有不同，最低和最高有50%的差别。

如果想多测试几次，可以用下面的shell命令
```
for i in {1..5}; do <command>; done
```
## 测试结果

| mode | bs | throughput | fio command |
| :--- | :--------: | :--------: | --- |
| random | 4KB | 8MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=4k --io_size=5G --rw=randread |
| sequential | 4KB | 18MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=4k --io_size=10G --rw=read |
| random | 8KB | 17MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=8k --io_size=10G --rw=randread |
| sequential | 8KB | 35MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=8k --io_size=12G --rw=read |
| random | 16KB | 38MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=16k --io_size=10G --rw=randread |
| sequential | 16KB | 65MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=16k --io_size=20G --rw=read |
| random | 32KB | 67MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=32k --io_size=25G --rw=randread |
| sequential | 32KB | 104MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=32k --io_size=40G --rw=read |
| random | 64KB | 115MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=64k --io_size=45G --rw=randread |
| sequential | 64KB | 192MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=64k --io_size=60G --rw=read |
| random | 128KB | 151MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=128k --io_size=60G --rw=randread |
| sequential | 128KB | 196MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=128k --io_size=80G --rw=read |
| random | 256KB | 178MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=256k --io_size=65G --rw=randread |
| sequential | 256KB | 192MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=256k --io_size=70G --rw=read |
| random | 512KB | 305MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=512k --io_size=19200M --rw=randread |
| sequential | 512KB | 394MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=512k --io_size=19200M --rw=read |
| random | 1024KB | 385MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=1024k --io_size=19200M --rw=randread |
| sequential | 1024KB | 587MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=1024k --io_size=19200M --rw=read |

## 分析

基本结论还是可以做出来的

1. SSD下，同一block size，对于throughput，顺序读比随机读有优势。在block size小时（64K），是一倍。当block size比较大，也是顺序读要好些。
2. block size越大，则throughput越大，block size小时（64K），block size大一倍，throughput也接近一倍。当block size比较大时，也会提高，但不是线性。大block size时，其throughput可以达到小的几十倍。
3. 因为block size和throughput的关系，可以推算出，IOPS在block size比较小的时候，是稳定的，而且比较高。当block size比较高时，IOPS开始降低，比如：bs=1024K下，IOPS在几百。同时，小block size下，IOPS都是几K。
4. SSD的性能表现不是很稳定，每次测试值都有偏差，会到20%左右。当block size接近1M时，这个不稳定非常明显，甚至会有1倍的差别。
5. 当刚copy一个大文件过来时，随后的read会性能较差，怀疑是gc导致。上面的测试数据，仅限于只读，是理想状况。

# write log pattern

## 命令

因为是Log，所以，我们可以确定是sequential，--rw=write，同时，我们只考虑经过os page cache，i.e., --direct=1

同时，既要用到page cache，同时又不能只有page cache，所以，--size有时需要远远超过os cache

在我的Mac上，cat /proc/meminfo | grep Cached，发现page cache是2G左右

范例
```
fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=8G --fsync=0 --bs=4k
```
NOTE: 
1. --end_fsync=1，最后文件写完，给一个fsync。
2. 如果direct=1，那么fsync的值不一定有效

我们主要测试，不同--bs下，fsync是0（不发出）,1或其他值的情况

## 数据

| bs | fsync | Throughputh |
| :-: | :-: | :-: |
| 4k | 0 | 196M/s |
| 4k | 1 | 6M/s |
| 4k | 2 | 18M/s |
| 4k | 4 | 34M/s |
| 4k | 8 | 57M/s |
| 8k | 0 | 232M/s |
| 8k | 1 | 9M/s |
| 8k | 2 | 31M/s |
| 8k | 4 | 60M/s |
| 8k | 8 | 110M/s |
| 16k | 0 | 228M/s |
| 16k | 1 | 28M/s |
| 16k | 2 | 54M/s |
| 16k | 4 | 107M/s |
| 16k | 8 | 155M/s |
| 32k | 0 | 249M/s |
| 32k | 1 | 73M/s |
| 32k | 2 | 110M/s |
| 32k | 4 | 133M/s |
| 64k | 0 | 212M/s |
| 64k | 1 | 111M/s |
| 64k | 2 | 182M/s |
| 128k | 0 | 225M/s |
| 128k | 1 | 171M/s |
| 256k | 0 | 224M/s |
| 256k | 1 | 164M/s |
| 512k | 0 | 248M/s |
| 512k | 1 | 211M/s |
| 1024k | 0 | 270M/s |
| 1024k | 1 | 239M/s |

## 总结

1. 当fysnc=0时，bs从4k到1024k,Throughput都差别不大，都是200M以上。这意味写盘都先到page cache里，然后由os来write back。一般而言，都是接近磁盘的写的最大带宽。
2. 当fsync=1时，是最慢的写盘操作。每一个bs写盘，都要flush & sync到SSD后才能继续。这相当于数据库系统里的每次写盘都sync的配置。是数据最安全的，但也是最慢的。其中，在4k，8k, 16k时，相比最大速度的写盘，有10倍以上的差别。很多数据库的页的大小，或者最小写盘单位，就是这三个单位。
3. 当fsync=1时，当bs比较大，比如512k, 1024k时，其写盘速度和最大带宽差别不大，因为当bs比较大时，SSD的并发优势将会被利用到。
4. 当bs比较小时，如4k，比较fsync的值从1到8，发现其对应的throughput也几乎是倍数增加。这也意味当小的写操作时，batch操作将会很好地利用到带宽。这也是很多数据库写盘操作里推崇batch的原因。

# Write of page cache with random 4k read

我们测试下面这种情况：首先write是log模式，同时全部走page cache，这样write log是最大效率。同时，另外一个进程（或线程）同时并发bs=4k的random read。然后看互相的影响。

在没有写的影响，纯粹的随机读，参考上面的测试数据，throughput是10M/s左右，i.e.，IOPS是20K以上。

在没有读的影响下，纯粹的顺序走page cache写，参考上面的测试数据，throughput是200-300M/s

然后，我们尝试下面的命令
先启动随机读， bs=4k, 不走cache(direct=1)
```
for i in `seq 1 10`; do fio --name=r --filename=readfile --ioengine=sync --rw=randread --io_size=50M --bs=4k --direct=1; done
```
紧跟着马上几乎同时启动顺序写，bs=1024k，走page cache, i.e., direct=0 & fsync=0
```
for i in `seq 1 30`; do fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=8G --fsync=0 --bs=1024k; done
```

我们发现，对于顺序走page cache的写，其throughput变化不太大。有时还在200-300M之间，有时就在100-200M之间。而且200-300M的概率要高于100-200M。最大值到了297M/s，最低到了130M/s。所以，估计也就最多20%的损失。

但对于随机读，并发时影响很大，大部分都在0.5M/s以下，最低时仅有0.2M/s。有接近50倍的差别。

# Write of page cache with random 1024k read

这个是模拟log写，和background做compaction时的随机大block size读的情况

先启动写
```
for i in `seq 1 30`; do fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=8G --fsync=0 --bs=1024k; done
```
然后几乎同时启动随机block size=1024k的读
```
for i in `seq 1 10`; do fio --name=r --filename=readfile --ioengine=sync --rw=randread --io_size=900M --bs=1024k --direct=1; done
```

我们发现，对于写，没有太大影响， 比上面的block size=4k影响还要小。

对于读，如果纯粹读，从上面的数据看，throughput可以到400M/s或500M/s这个量级。但在这个测试中的并发中，发现基本throughput都降低到20M/s左右，有20多倍的降低。

所以，通过两个测试，可以知道，对于经过page cache的log写（block size=1024k），其throughput没有太大影响。影响大的是读（我们只考虑随机读，这也是生产环境的真实现象），不管是block size = 4k，还是block size = 1024k，和纯粹的比，都有大幅的降低，20多倍，或者40多倍。

不过，好在，读我们是很容易做load balance的，所以写不受太大影响，是好事。

真正对于写的优化是：log写有两个，一个是注入ingest写，一个是background compaction的写。如何分配并发，同时如何降低write amplification，才是写优化的两个关键。

# read multi thread vs io depth

[对这篇文章Figure3](https://www.usenix.org/system/files/conference/fast16/fast16-papers-lu.pdf)，里面的东西有所怀疑

1. sequential和random差距这么大(但多线程后，throughput有趋同)
2. sequential在block size从小到大时，throughput几乎无变化，非常奇怪
2. 多线程的因素，到底是多线程，还是io任务队列足够多（需要做单线程多io任务， 和多线程的比较）

当前还不能测试，因为发现自己机器的Linux下的io submitted queue length & io completed queue length，在1-2之间
怀疑可能是：
1. Multipass虚拟机导致，即driver并没有使用到NVMe的接口（否则queue length应该可以上去）
2. 还有几十自己的Mac OS的SSD的性能确实差，实际的length（注意：不是libaio下的iodepth）

```
libao下多任务（怀疑libaio其实也是多线程）
fio --name=test --filename=tfile --size=400M --rw=randread --ioengine=libaio --direct=1 --bs=4k --iodepth=1
多线程
fio --name=test --filename=tfile --size=400M --rw=randread --ioengine=sync --direct=1 --bs=4k --numjobs=4 --thread --group_reporting=1
```
