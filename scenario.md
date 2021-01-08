
# 测试环境

在Mac上的虚拟Linux(基于MultiPass)

如果是新购置的SSD或purge过的SSD，请预热读写至少超过24小时以上。避免SSD的clifff现象。

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

我是下载了一个[Ubuntu 20的Desktop安装版本，有近3G大小](https://releases.ubuntu.com/20.04/)。

```
curl <URI> -o tfile
```

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

# 纯Read

## 基本Read：单线程，同步

我们用最基本的read，即ioengine=sync，同时不受page cache影响，所以direct=1。然后比较随机读rw=randread，以及顺序读rw=read，比较block size在不同值下的throughput.

### 测试注意

如果我们尝试将bs改为其他值，包括8k, 16k, 32k，64k，然后比较随机读和顺序读，我们得到下表

NOTE: 
1. 测试文件大些，如果测试文件过小，或者用了比较小的size值，会导致throughput明显过高（可达1倍）。如上文，测试文件最好是安装包等有压缩数据的文件，也比较真实模拟生产环境。

2. 我们的测试时间需要足够长，至少5分钟以上，否则，请修改io_size参数。如果时间过短，SSD内部的缓存可能起很大作用，有时结果会有2倍的差别。

3. 建议多测几次，取中间值或概率较多的值。即使每次几分钟，每次的值都有不同，最低和最高有时接近100%的差别（这个不稳定让人疑惑）。

如果想多测试几次，可以用下面的shell命令
```
for i in {1..5}; do <command>; done
```
### 测试结果

| mode | bs | Tp | fio command |
| --- | -------- | -------- | --- |
| random | 4KB | 10.0MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=4k --io_size=5G --rw=randread |
| sequential | 4KB | 23.7MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=4k --io_size=10G --rw=read |
| random | 8KB | 21.6MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=8k --io_size=10G --rw=randread |
| sequential | 8KB | 42.6MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=8k --io_size=12G --rw=read |
| random | 16KB | 44.3MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=16k --io_size=10G --rw=randread |
| sequential | 16KB | 84.4MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=16k --io_size=20G --rw=read |
| random | 32KB | 78.2MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=32k --io_size=25G --rw=randread |
| sequential | 32KB | 149MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=32k --io_size=40G --rw=read |
| random | 64KB | 152MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=64k --io_size=45G --rw=randread |
| sequential | 64KB | 251MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=64k --io_size=60G --rw=read |
| random | 128KB | 171MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=128k --io_size=60G --rw=randread |
| sequential | 128KB | 378MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=128k --io_size=80G --rw=read |
| random | 256KB | 247MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=256k --io_size=65G --rw=randread |
| sequential | 256KB | 434MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=256k --io_size=70G --rw=read |
| random | 512KB | 279MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=512k --io_size=70G --rw=randread |
| sequential | 512KB | 417MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=512k --io_size=70G --rw=read |
| random | 1024KB | 297MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=1024k --io_size=70G --rw=randread |
| sequential | 1024KB | 533MB/s | fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=1024k --io_size=80G --rw=read |

### 分析

基本结论还是可以做出来的

1. random和sequential的对比：同一block size，对于throughput，顺序读比随机读有优势。在block size小时（小于或等于64k），是一倍的关系。在bs > 128k，也是sequential比random要更好一些(50%左右的提高)。感觉上，sequential好像是预先读出（因为SSD内部也有SDRAM的cache），因此加快。

2. 单个模式下block size变化规律：block size越大，则throughput越大，block size小时（小于或等于64k），block size大一倍，throughput也接近一倍。最大block size，i.e., 1024k，相比最小的block size，i.e., 4k，其throughput相比可以20-30倍的差别，i.e., random或sequential模式下bs=4k vs bs=1024k。 这比较符合SSD的工作原理，即并发导致高速（parallelism for performance），而block size比较高时，利于SSD内部做并发。而block size到了128k以上时，并发的边际效应开始降低。

3. 因为block size和throughput的关系，可以推算出，IOPS在block size比较小的时候，比较高，在k级别。当block size比较高时，IOPS开始降低，比如：bs=1024K下，IOPS在几百。

4. 对于同一mode以及某固定block size, SSD的性能表现不是很稳定，每次测试值都有偏差。即使我们采用5分钟以上的运行时间，这个差别仍存在。20%的差别是很正常的。甚至有时几倍数的差别（可以看fio的disk utilization这个参数，越高越容易获得高的throughput，但这个参数的意义以及和哪些东西相关，不明）。不过，从统计上看，如果足够多的次数，那么出现概率较高的throughput，还是相对稳定。不稳定的因素不明，只能怀疑是SSD内部的算法，比如：SSD内部的cache的管理。

## read block size = 4k时，iodepth的影响

### 测试结果

我们测试iodepth的影响，因此，ioengine需要用libaio，同时必须保证direct=1，否则libaio没有用。

io depth == --iodepth; io batch == --iodepth_batch, bs == bs or block size, Tp == throughput

| mode | io depth | io batch | bs | Tp | fio command |
| --- | ----- | -- | -------- | -------- | --- |
| random | 1 | 1 | 4K | 10MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=5G --rw=randread --iodepth=1 |
| random | 2 | 1 | 4K | 13MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=5G --rw=randread --iodepth=2 |
| random | 4 | 1 | 4K | 14MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=5G --rw=randread --iodepth=4 |
| random | 4 | 4 | 4K | 12MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=5G --rw=randread --iodepth=4 --iodepth_batch=4 |
| random | 32 | 1 | 4K | 15MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=5G --rw=randread --iodepth=32  |
| random | 32 | 8 | 4K | 13MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=5G --rw=randread --iodepth=32 --iodepth_batch=8 |
| sequential | 1 | 1 | 4K | 16MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=5G --rw=read --iodepth=1 |
| sequential | 2 | 1 | 4K | 27MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=10G --rw=read --iodepth=2 |
| sequential | 4 | 1 | 4K | 39MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=10G --rw=read --iodepth=4 |
| sequential | 4 | 4 | 4K | 46MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=10G --rw=read --iodepth=4 --iodepth_batch=4 |

并发(multi process or multi thread)情况下的结果
```
fio --name=t --filename=tfile --ioengine=sync --direct=1 --bs=4k --io_size=500M --rw=randread --iodepth=16 --numjobs=16 --group_reporting
```
Throughput = 12.4MB/s, IOPS = 3016

```
fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=500M --rw=randread --iodepth=16 --numjobs=16 --group_reporting
```
Throughput = 11.3MB/s, IOPS = 2768

### 分析

我们只测试了block size=4k。

1. 对于random模式，iodepth的影响不大，可以认为接近于0。注：采用了并发模式，--numjobs=16 --group_reporting，结果差不多。

2. 对于sequential模式，iodepth有一定的影响，比如：iodepth=4时，是iodepth=1的几乎3倍。如果用SSD内部的cache去解释，似乎可以解释得通（包括对比random模式）。


下面会有其他情况下的测试，包括：block size不止4k， iodepth和线程数的同时影响。

## random read multi thread 以及 io depth

### WiscKey里的测试

[对这篇文章 WiscKey- Separating Keys from Values in SSD-conscious Storage， Figure3](https://www.usenix.org/system/files/conference/fast16/fast16-papers-lu.pdf)，里面的东西有所怀疑，因为从图示分析得到下面三点，

1. sequential在block size从小到大时，throughput几乎无变化。有两种可能，其一，就是queued depth很大，这样小的block size即使咋单线程下，也可以因为queue汇合成大的block size；但注意：我的Mac上的SSD没有这个特性，也许其他企业级的SSD如此；其二，就是多线程效果，这样并发的请求达到和同样的效果。

2. random read随着block size增大，不管是单线程，还是多线程，都是增大的。但多线程，有进一步放大的效果。在一定线程数和不太大的block size下，对于random read，也可以达到和sequential read一样的最高的throughput。图中单线程只到block size=256K这种情况，其throughput也达到最大值的近60%，如果能到block size=1M，怀疑能接近最大的带宽。

3. 多线程的因素，到底是多线程，还是io任务队列足够多（需要做单线程多io任务， 和多线程的比较）

但我自己的验证有些不同，见下表

### 我自己Mac测试情况

我自己在Mac上做上面的测试 （注意：用```for i in {1..5}; do <>; done，因为一次的数据可能会有很大的倍数级别的偏差）

| bs | threads | iodepth | throughtput | command |
| -- | -- | -- |  -- | -- |
| 4k | 1 | 1 | 10.5MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=1G --rw=randread --iodepth=1 --numjobs=1 --thread --group_reporting |
| 4k | 1 | 32 | 16.8MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=1G --rw=randread --iodepth=32 --numjobs=1 --thread --group_reporting |
| 4k | 32 | 32 | 20.3MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=4k --io_size=100M --rw=randread --iodepth=32 --numjobs=32 --thread --group_reporting |
| 16k | 1 | 1 | 34.5MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=16k --io_size=4G --rw=randread --iodepth=1 --numjobs=1 --thread --group_reporting |
| 16k | 1 | 32 | 58.6MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=16k --io_size=4G --rw=randread --iodepth=32 --numjobs=1 --thread --group_reporting |
| 16k | 32 | 32 | 77.4MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=16k --io_size=400M --rw=randread --iodepth=32 --numjobs=32 --thread --group_reporting |
| 64k | 1 | 1 | 115MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=64k --io_size=16G --rw=randread --iodepth=1 --numjobs=1 --thread --group_reporting |
| 64k | 1 | 32 | 195MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=64k --io_size=16G --rw=randread --iodepth=32 --numjobs=1 --thread --group_reporting |
| 64k | 32 | 32 | 368MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=64k --io_size=1600M --rw=randread --iodepth=32 --numjobs=32 --thread --group_reporting |
| 256k | 1 | 1 | 233MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=256k --io_size=64G --rw=randread --iodepth=1 --numjobs=1 --thread --group_reporting |
| 256k | 1 | 32 | 442MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=256k --io_size=64G --rw=randread --iodepth=32 --numjobs=1 --thread --group_reporting |
| 256k | 32 | 32 | 1128MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=256k --io_size=7G --rw=randread --iodepth=32 --numjobs=32 --thread --group_reporting |
| 1024k | 1 | 1 | 264MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=1024k --io_size=128G --rw=randread --iodepth=1 --numjobs=1 --thread --group_reporting |
| 1024k | 1 | 32 | 1052MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=1024k --io_size=128G --rw=randread --iodepth=32 --numjobs=1 --thread --group_reporting |
| 1024k | 32 | 32 | 1976MB/s | fio --name=t --filename=tfile --ioengine=libaio --direct=1 --bs=1024k --io_size=15G --rw=randread --iodepth=32 --numjobs=32 --thread --group_reporting |

结论:

1. 当threads=1时，iodetpht=1, 这里所用的libaio，和上面的sync read比，差别不大。

2. iodepth和thraeds，都起一定作用，即利于SSD并发从而得到更好的性能。

### 网上一个Samsung SSD的测试报告

另外还有一个测试报告，[Samsung 960 Pro](https://www.anandtech.com/show/10754/samsung-960-pro-ssd-review)。

从上面这个测试报告，可以看出，如果QD=1，Block Size = 4KB时，其IOPS = 14K, 也就是说throughput是56MB/s。相当于Sequential Read的3500MB/s的 1/63。

对于写，QD=1, Block Size = 4KB，其IOPS是50K，即throughtput是200MB/s, 是Sequential Write的2100MB/s的 1/11。

而当QD=32时，对于读，Block Size = 4KB，其IOPS是440K，即throughput是1760MB/s，因此对比Sequential，比例变为 1/2。

而当QD=32时，对于写，Block Size = 4KB，其IOPS是360K，即throughput是1440MB/s，因此对比Sequential，比例变为 1/1。

从这个报告看出，block size小时，如果queue depth比较多，其性能，和sequential或block size很大时非常接近，即SSD的并发得到充分利用。

这和多线程一起工作，是一个道理。

所以，我们再测试block size比较小的4KB时，一定要注意这个并发性，否则，就没有太大的差别，容易搞错测试的本质。

# 纯Write

## 基本Write：单线程，同步

### 命令说明

因为是Log，所以，我们可以确定是sequential，--rw=write，同时，我们只考虑经过os page cache的writeback写，i.e., --direct=0

同时，既要用到page cache，同时又不能只有page cache，所以，有可能的话，--size需要远远超过os cache。

在我的Mac上，cat /proc/meminfo | grep Cached，发现page cache是2G左右

范例
```
fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=8G --fsync=0 --bs=4k
```
NOTE: 
1. --end_fsync=1，最后文件写完，保证给一个fsync，因为有时fsync=0 (比如上面的direct=0等价于fsync=0)

2. 如果direct=1，那么fsync参数不一定有效，[参考fio的文档](https://fio.readthedocs.io/en/latest/fio_doc.html#)

3. 如果测试时间比较短（只有几十秒），请用多次```for i in {1..5}; do <command>; done```，然后最高频率的throughput作为其结果

我们主要测试，不同block size下，fsync是0（不发出）,1（相当于sync）或其他值（相当于batch commit）的情况

### 测试结果

| bs | fsync | Tp | fio command |
| :-: | :-: | -- | -- |
| 4k | 0 | 288M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=0 --bs=4k |
| 4k | 1 | 5M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=1G --fsync=1 --bs=4k |
| 4k | 2 | 10M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=2G --fsync=2 --bs=4k |
| 4k | 4 | 15M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=3G --fsync=4 --bs=4k | 
| 4k | 8 | 27M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=6G --fsync=8 --bs=4k |
| 8k | 0 | 284M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=0 --bs=8k |
| 8k | 1 | 9M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=2G --fsync=1 --bs=8k |
| 8k | 2 | 15M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=5G --fsync=2 --bs=8k |
| 8k | 4 | 29M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=5G --fsync=4 --bs=8k |
| 8k | 8 | 34M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=8 --bs=8k |
| 16k | 0 | 257M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=0 --bs=16k |
| 16k | 1 | 16M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=4G --fsync=1 --bs=16k |
| 16k | 2 | 18M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=2 --bs=16k |
| 16k | 4 | 35M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=4 --bs=16k |
| 16k | 8 | 75M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=8 --bs=16k |
| 32k | 0 | 255M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=0 --bs=32k |
| 32k | 1 | 22M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=5G --fsync=1 --bs=32k |
| 32k | 2 | 35M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=2 --bs=32k |
| 32k | 4 | 63M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=4 --bs=32k |
| 64k | 0 | 262M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=0 --bs=64k |
| 64k | 1 | 39M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=1 --bs=64k |
| 64k | 2 | 66M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=2 --bs=64k |
| 128k | 0 | 269M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=0 --bs=128k |
| 128k | 1 | 77M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=1 --bs=128k |
| 256k | 0 | 259M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=0 --bs=256k |
| 256k | 1 | 164M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=1 --bs=256k |
| 512k | 0 | 275M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=7G --fsync=0 --bs=512k |
| 512k | 1 | 211M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=8G --fsync=1 --bs=512k |
| 1024k | 0 | 273M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=12G --fsync=0 --bs=1024k |
| 1024k | 1 | 239M/s | fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=8G --fsync=1 --bs=1024k |

### 对比一下libaio

只考虑block size=4k，同时只是最后用sync，i.e., fsync=0(direct=1) and end_fsync=1。

| bs | iodepth | Tp | fio command |
| :-: | :-: | -- | -- |
| 4k | 1 | 18M/s | fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=7G --bs=4k --iodepth=1 |
| 4k | 2 | 26M/s | fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=7G --bs=4k --iodepth=2 |
| 4k | 4 | 35M/s | fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=7G --bs=4k --iodepth=4 |
| 4k | 8 | 37M/s | fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=7G --bs=4k --iodepth=8 |
| 4k | 16 | 41M/s | fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=7G --bs=4k --iodepth=16 |

## 总结

1. 当fysnc=0时，bs从4k到1024k,Throughput都差别不大，都是近300M。这意味写盘都先到page cache里，然后由os来write back。一般而言，都是接近磁盘的写的最大带宽。

2. 当fsync=1时，是最慢的写盘操作。每一个bs写盘，都要flush & sync到SSD后才能继续。这相当于数据库系统里的每次写盘都sync的配置。是数据最安全的，但也是最慢的。其中，在4k，8k, 16k时，相比fsync=0或fsync!=0但bs=1024k的写盘，有10倍以上的差别。很多数据库的页的大小，或者最小写盘单位，就是这三个单位。

3. 当fsync=1时，当bs比较大，比如512k, 1024k时，其写盘速度和最大带宽差别不大，因为当bs比较大时，SSD的并发优势将会被充分利用到。同时，这也给设计带来一个技巧，就是batch write。收集一批小的写，然后集中后用一个比较大的block size写入，这时，其性能基本是最大值，和OS page cache的效果差不多。

4. 当bs比较小时，如4k，比较fsync的值从1到8，发现其对应的throughput也几乎是50%-100%的增加。这也意味当小的写操作时，batch操作将会很好地利用到带宽。这也是很多数据库写盘操作里推崇batch的原因。

## 多线程和io dpeth下的Write

由于是RocksDB，我们只考虑block szie = 1024k的情况 (以下做5次，取中间值)

| threads | io depth | throughtput | command |
| -- | -- | -- | -- |
| 1 | 1 | 167MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=40G --bs=1024k --iodepth=1 --numjobs=1 --thread --group_reporting |
| 1 | 4 | 199MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=40G --bs=1024k --iodepth=4 --numjobs=1 --thread --group_reporting |
| 1 | 32 | 213MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=40G --bs=1024k --iodepth=32 --numjobs=1 --thread --group_reporting |
| 2 | 1 | 321MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=40G --bs=1024k --iodepth=1 --numjobs=2 --thread --group_reporting |
| 2 | 32 | 366MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=40G --bs=1024k --iodepth=32 --numjobs=2 --thread --group_reporting |
| 4 | 1 | 591MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=40G --bs=1024k --iodepth=1 --numjobs=4 --thread --group_reporting |
| 4 | 4 | 647MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=40G --bs=1024k --iodepth=4 --numjobs=4 --thread --group_reporting |
| 4 | 32 | 637MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=40G --bs=1024k --iodepth=32 --numjobs=4 --thread --group_reporting |
| 16 | 1 | 1476MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=40G --bs=1024k --iodepth=1 --numjobs=16 --thread --group_reporting |
| 16 | 4 | 1653MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=40G --bs=1024k --iodepth=4 --numjobs=16 --thread --group_reporting |
| 32 | 1 | 2345MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=10G --bs=1024k --iodepth=1 --numjobs=32 --thread --group_reporting |
| 32 | 4 | 2197MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=10G --bs=1024k --iodepth=4 --numjobs=32 --thread --group_reporting |
| 32 | 32 | 1376MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=10G --bs=1024k --iodepth=32 --numjobs=32 --thread --group_reporting |
| 64 | 1 | 2340MB/s | fio --name=w --filename=wfile --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=10G --bs=1024k --iodepth=1 --numjobs=64 --thread --group_reporting |

以上数据惊人，throughput甚至达到了GB/s级别，而且全部和thread数量增加相关。有个怀疑是：其实多线程写，是多线程操作SSD内部的SDRAM，即第一个线程顺序写文件，留下了很多cache在SSD内部的SDRAM里，后面的线程利用了这个cache，虽然还需要再写一遍，但由于是同一文件同一位置，所以速度可以大大提高（甚至可以优化成0写）。

所以，做个修正，让每个线程写不同的文件，然后并发线程，看效果如何，见下表

| threads | io depth | throughtput | command |
| -- | -- | -- | -- |
| 4 | 1 | 189MB/s | fio --name=w  --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=4G --io_size=10G --bs=1024k --iodepth=1 --numjobs=4 --thread --group_reporting |
| 4 | 4 | yyy | fio --name=w  --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=4G --io_size=10G --bs=1024k --iodepth=4 --numjobs=4 --thread --group_reporting |


补充：我们看一下block size是1M，但是random write的情况：

| threads | io depth | throughtput | command |
| -- | -- | -- | -- |
| 1 | 1 | 155MB/s | fio --name=w --filename=wfile --rw=randwrite --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=40G --bs=1024k --iodepth=1 --numjobs=1 --thread --group_reporting |
| 1 | 4 | 198MB/s | fio --name=w --filename=wfile --rw=randwrite --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=40G --bs=1024k --iodepth=4 --numjobs=1 --thread --group_reporting |
| 4 | 1 | 182MB/s | fio --name=w --filename=wfile --rw=randwrite --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=20G --bs=1024k --iodepth=1 --numjobs=4 --thread --group_reporting |
| 4 | 4 | 176MB/s | fio --name=w --filename=wfile --rw=randwrite --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=20G --bs=1024k --iodepth=4 --numjobs=4 --thread --group_reporting |
| 8 | 1 | 169MB/s | fio --name=w --filename=wfile --rw=randwrite --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=7G --bs=1024k --iodepth=1 --numjobs=8 --thread --group_reporting |
| 8 | 4 | 169MB/s | fio --name=w --filename=wfile --rw=randwrite --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=15G --io_size=7G --bs=1024k --iodepth=4 --numjobs=8 --thread --group_reporting |


# Write mix with Read 

## Write of page cache with random 4k read

我们测试下面这种情况：首先write是log模式，同时全部走page cache，这样write log是最大效率。同时，另外一个进程（或线程）同时并发bs=4k的random read。然后看互相的影响。

在没有写的影响，纯粹的随机读，参考上面的测试数据，throughput是10M/s左右，i.e.，IOPS是2K以上。

在没有读的影响下，纯粹的顺序走page cache写，参考上面的测试数据，throughput是200-300M/s

然后，我们尝试下面的命令
先启动随机读， bs=4k, 不走cache(direct=1)
NOTE: [rfile来自一个不到1G的安装包](https://releases.ubuntu.com/20.04/)
```
for i in {1..10}; do fio --name=r --filename=rfile --ioengine=sync --rw=randread --io_size=50M --bs=4k --direct=1; done
```
紧跟着马上几乎同时启动顺序写，bs=1024k，走page cache, i.e., direct=0 & fsync=0
```
for i in {1..30}; do fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=6G --fsync=0 --bs=1024k; done
```

我们发现，对于顺序走page cache的写，其throughput变化不太大。有时还在200-300M之间，有时就在100-200M之间。而且200-300M的概率要高于100-200M。最大值到了297M/s，最低到了130M/s。所以，估计也就30%的损失。

但对于随机读，并发时影响很大，大部分都在0.5M/s以下，最低时仅有0.2M/s。有接近50倍的差别。

## Write of page cache with random 1024k read

这个是模拟log写，和background做compaction时的随机大block size读的情况

先启动写
```
for i in {1..30}; do fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=5G --fsync=0 --bs=1024k; done
```
然后几乎同时启动随机block size=1024k的读
```
for i in {1..10}; do fio --name=r --filename=rfile --ioengine=sync --rw=randread --io_size=900M --bs=1024k --direct=1; done
```

我们发现，对于写，没有太大影响， 比上面的block size=4k影响还要小。

对于读，如果纯粹读，从之前的数据看，throughput可以到近200M。但在这个测试中的并发中，发现基本throughput在20M多或30M多，有6-9倍的降低。

## 分析和结论

所以，通过两个测试，可以知道，对于经过page cache的log写（block size=1024k），其throughput没有严重的影响（30%的影响）。影响大的是读（我们只考虑随机读，这也是生产环境的真实情况），不管是block size = 4k，还是block size = 1024k，和纯粹的比，都有大幅的降低。

这是因为，page cache的writeback，一次性占用整个SSD的带宽，然后中间见缝插针地，来了一些random read。如果random read的block size比较小的话，相应的io数会多一些，因此对writeback的影响也相应大一些。但不管如何，writeback都是主要的带宽使用者，剩下的边角余料，才能供给random read.

不过，好在，读我们是很容易做load balance的，所以写不受太大影响，是好事。

真正对于写的优化是：log写有两个，一个是注入ingest写，一个是background compaction的写。如何分配并发，同时如何降低write amplification，才是写优化的两个关键。

