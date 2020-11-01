
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
dd if=/dev/zero of=testfile bs=1M count=2048
```

但这个文件有一个问题，就是内容都是0，这不利于测试真实的情况，因为SSD是根据文件内容的熵，散列到内部的cell里进行并发处理。

所以，用真实的数据文件，或者下载一个大小相当的文件包（最好是压缩的文件，比较接近真实的情况）

我是下载了一个Ubuntu 20的Desktop安装版本，有近3G大小。

## read & direct & invalidate

对于read，如果direct=1，那么page cache将不启作用。如果direct=0，但invalidate=1，那么page cache的作用是零

## 清page cache

```
sudo -i
sync; echo 1 > /proc/sys/vm/drop_caches
```

## 查看page cache

```
cat /proc/meminfo
```

然后看```Cached:```这一项，如果只有很少的数量，比如几十M，那么就说明page cache已经清楚干净了

## page cache的热身

```
fio --name=test --filename=testfile --rw=read --size=2048M --ioengine=sync --bs=4k --direct=0
```

然后用上面的查看page cache的命令查看，可以看到```Cached:```到了几个G，说明文件已经被缓存到page cache里了

## read, direct, invalidate的对比

如果read不经过page cache，i.e., direct=1，

```
fio --name=test --filename=testfile --rw=randread --size=200M --ioengine=sync --bs=4k --direct=1
```
在我的机器上，throughtput=15MB/s

如果设置direct=0，经过page cache，你会发现结果差不多
```
fio --name=test --filename=testfile --rw=randread --size=200M --ioengine=sync --bs=4k --direct=0
```
这是因为还有一个参数invalidate，这个参数是每次读前，将对应的cache先清除，所以，就和direct=1一样了

如果我们增加设置invaliddate，如下
```
fio --name=test --filename=testfile --rw=randread --size=200M --ioengine=sync --bs=4k --direct=0 --invalidate=0
```
就会发现throughput=2GB/s左右。这是因为基本都是从内存读到数据（我们之前有做热身）
如果将随机读改为顺序读（同时不启用page cache），命令如下
```
fio --name=test --filename=testfile --rw=read --size=200M --ioengine=sync --bs=4k --direct=1
```
这时throughput=17MB/s，说明顺序读和随机读的性能差别不大。

## read各种block size下的对比

如果我们尝试将bs改为其他值，包括8k, 16k, 32k，64k，然后比较随机读和顺序读，我们得到下表

NOTE: 
1. 我们的测试时间需要足够长，至少10秒以上，否则，请修改size和io_size参数 (size表示从文件里多少字节，io_size表示总共多少字节)
2. 面的测试，每次值都有一定抖动，偏差可达20%，我们连续测试5次，取出现概率较多的值

| mode | block size | throughput | fio command |
| :--------------------------: | :--------: | :--------: | --- |
| random | 4KB | 21MB/s | fio --name=test --filename=testfile --rw=randread --size=400M --ioengine=sync --bs=4k --direct=1 |
| sequential | 4KB | 25MB/s | fio --name=test --filename=testfile --rw=read --size=400M --ioengine=sync --bs=4k --direct=1 |
| random | 8KB | 43MB/s | fio --name=test --filename=testfile --rw=randread --size=600M --ioengine=sync --bs=8k --direct=1 |
| sequential | 8KB | 50MB/s | fio --name=test --filename=testfile --rw=read --size=600M --io_size=1000M --ioengine=sync --bs=8k --direct=1 |
| random | 16KB | 74MB/s | fio --name=test --filename=testfile --rw=randread --size=1000M --ioengine=sync --bs=16k --direct=1 |
| sequential | 16KB | 98MB/s | fio --name=test --filename=testfile --rw=read --size=1000M --io_size=1500M --ioengine=sync --bs=16k --direct=1 |
| random | 32KB | 161MB/s | fio --name=test --filename=testfile --rw=randread --size=1000M --io_size=2500M --ioengine=sync --bs=32k --direct=1 |
| sequential | 32KB | 191MB/s | fio --name=test --filename=testfile --rw=read --size=1000M --io_size=3000M --ioengine=sync --bs=32k --direct=1 |
| random | 64KB | 290MB/s | fio --name=test --filename=testfile --rw=randread --size=1000M --io_size=5000M --ioengine=sync --bs=64k --direct=1 |
| sequential | 64KB | 343MB/s | fio --name=test --filename=testfile --rw=read --size=1000M --io_size=6000M --ioengine=sync --bs=64k --direct=1 |
| random | 128KB | 472MB/s | fio --name=test --filename=testfile --rw=randread --size=1000M --io_size=8000M --ioengine=sync --bs=128k --direct=1 |
| sequential | 128KB | 553MB/s | fio --name=test --filename=testfile --rw=read --size=1000M --io_size=10000M --ioengine=sync --bs=128k --direct=1 |
| random | 256KB | 632MB/s | fio --name=test --filename=testfile --rw=randread --size=1000M --io_size=12000M --ioengine=sync --bs=256k --direct=1 |
| sequential | 256KB | 677MB/s | fio --name=test --filename=testfile --rw=read --size=1000M --io_size=12000M --ioengine=sync --bs=256k --direct=1 |
| random | 512KB | 534MB/s | fio --name=test --filename=testfile --rw=randread --size=1000M --io_size=12000M --ioengine=sync --bs=512k --direct=1 |
| sequential | 512KB | 543MB/s | fio --name=test --filename=testfile --rw=read --size=1000M --io_size=12000M --ioengine=sync --bs=512k --direct=1 |
| random | 1024KB | 587MB/s | fio --name=test --filename=testfile --rw=randread --size=1000M --io_size=12000M --ioengine=sync --bs=1024k --direct=1 |
| sequential | 1024KB | 602MB/s | fio --name=test --filename=testfile --rw=read --size=1000M --io_size=12000M --ioengine=sync --bs=1024k --direct=1 |

基本结论还是可以做出来的

1. SSD下，同一block size，对于throughput，顺序读和随机读差别不大，基本一个数量级，顺序读略高，即使算上偏差，也到不了1倍的量级。
2. block size越大，则throughput越大，前期基本接近线性，即block size大一倍，throughput也接近一倍。到了128K后，基本就持平，而且256K左右是最高峰。
3. SSD的性能表现不是很稳定，每次测试值都有偏差，会到20%左右。当刚copy一个大文件过来时，随后的read会性能较差，怀疑是gc导致。上面的测试数据，仅限于只读，是理想状况。

## read multi thread vs io depth

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
fio --name=test --filename=testfile --size=400M --rw=randread --ioengine=libaio --direct=1 --bs=4k --iodepth=1
多线程
fio --name=test --filename=testfile --size=400M --rw=randread --ioengine=sync --direct=1 --bs=4k --numjobs=4 --thread --group_reporting=1
```
