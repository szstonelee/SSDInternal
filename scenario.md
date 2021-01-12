
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

我们需要将之改为none模式的IO Scheduler，选用比较适合SSD的none模式。
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

我是下载了一个[Ubuntu 20的Desktop安装版本，有近3G大小](https://releases.ubuntu.com/20.04/)。我把它命名为tfile

```
curl <URI> -o tfile
```

同时发现，如果文件过小（比如百兆大小文件针对G量级文件），有1倍的数据差异。所以，建议部署接近Production的数据量，比如针对Rocksdb，部署多个文件，总量可达TB级别，参考fio的Target file/device的相关说明。

为了产生大文件，可以在tfile基础上，形成一个几倍大小的total（10G or 20G）
```
cp tfile total
cat tfile >> total
cat tfile >> total
...
```

## fio :: direct 参数 

如果direct=1，不管是read还是write，那么page cache将不启作用（write bypass page cache同时，会让相应的page失效）。

如果direct=0，将使用到OS page cache。这很复杂。还有两个参数可能影响，[一个是invalidate，一个是pre_read](https://fio.readthedocs.io/en/latest/fio_man.html#cmdoption-arg-invalidate)

下面的测试中，如果我们不想使用page cache，direct总是1。但如果我们想看page cache的write back功能，我们设置direct = 0，一般同时[fsync = 0](https://fio.readthedocs.io/en/latest/fio_man.html#cmdoption-arg-fsync)（也偶尔测试批sync的）

### 清page cache

```
sudo -i
sync; echo 1 > /proc/sys/vm/drop_caches
```

### 查看page cache

```
cat /proc/meminfo | grep Cached
```

然后看```Cached:```这一项，如果只有很少的数量，比如几十M，那么就说明page cache已经清除干净了

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

上面那个page cache能有效，是因为randread两次执行的随机数时一样的，如果我们设置不同的随机数，i.e., --randrepeat=0，我们会发现page cache的作用没了（或准确地说，作用变小了，因为读的是200M，文件tfile超过2G，随机读到同一page的机会不大）
```
fio --name=test --filename=tfile --rw=randread --io_size=200M --ioengine=sync --bs=4k --direct=0 --invalidate=0 \
--randrepeat=0
```

如果将随机读改为顺序读（同时不启用page cache），命令如下
```
fio --name=test --filename=tfile --rw=read --io_size=200M --ioengine=sync --bs=4k --direct=1
```
这时throughput=17MB/s，说明顺序读是随机读一倍。

## sync vs aio 以及 direct = 1 or 0

fio我们使用两种engine，一个是sync，一个是libaio。

sync很好理解。如果direct = 0，表示用到OS page cache。如果direct = 1，则表示[尽量不用到page cache](https://ext4.wiki.kernel.org/index.php/Clarifying_Direct_IO%27s_Semantics)，i.e., read through or write through page cache.

AIO是Linux基于底层IO系统的async IO。比较有趣的是，Linux在block driver这一层，实际上是不支持async的，即AIO实际是在底层的非异步系统上包装了一层，以提供应用程序的异步接口。尽管AIO的部分组件已经是kernal的一部分，但逻辑理解上，你可以将AIO当做应用层上的事情，即Linux kernal IO是不支持异步模式的。Windows有不同，因为Windows有IO Completed这个接口，实现了真正的基于kernal的异步模式。但可惜的是，在服务器领域，Windows已经出局了。

当使用AIO时，必须将direct设置为1，即不允许使用page cache。[参考这里](https://fio.readthedocs.io/en/latest/fio_man.html#i-o-engine)

为什么？因为如果所读的页基于OS cache，那么它随时有可能被OS evict，i.e., 对任何一application，OS不保证上一指令已经读到的页在下一个指令还存在于内存，这将使异步无法建造或失去意义。因为我们用异步的目的，就是为了当时调用时工作线程calling后不用去阻塞block，可以去做其他工作，之后某个合适的时刻（比如 poll or epoll）在后面某个异步的event事件里能得到基于内存的保证实时操作的性能，i.e. non-blocking。所以，你可以理解AIO使用了自己特定的一套IO buffer管理和线程同步模式。这就好比MySQL和PostgreSQL对于buffer的管理理念不一样，MySQL尽可能用自己的buffer管理IO，而PostgreSQL则相信OS page cache。

如果我们用AIO，又设置direct = 0会如何？[参考Linux的帮助](http://lse.sourceforge.net/io/aio.html)，这将使AIO报错或悄悄转为sync模式。

## flush and sync

很多时候，flush和sync在IO这一层被混用，其实它是两个概念。

flush表示的是，IO数据，从应用层到了kernal层，i.e., OS page cache or block driver buffer。

而sync，表示的是数据100%落到了磁盘上，i.e., disk给了response，拍胸脯保证刚才要写的数据安全入盘了（但disk可能配置错误导致失败，比如：RAID卡中设置了write cache，或者硬件出问题，比如Disk Controller卡上的电池没电）。

为了准确，我们尽可能用sync这个概念，少用flush这个词。在Linux API里，可以找到sync()和write()，但没有flush()。有fflush()，但那其实是write()。

# 纯read

## 基本Read：单线程，同步

我们用最基本的read，即ioengine=sync，同时不受page cache影响，所以direct=1。然后比较随机读rw=randread，以及顺序读rw=read，比较block size在不同值下的throughput.

### 测试注意

1. 我们的测试时间需要足够长，至少分钟以上，否则，请修改io_size参数。同时多次，避免一次的偶然。从测试数据看，每次测试还是有一定的偏差，但多次后，相对稳定。

如果想多测试几次，可以用下面的shell命令
```
for i in {1..5}; do <command>; done
```

2. 测试文件大小有一定影响。如果用小的文件tfile(2.6G），偏差会比较大，有时会到1倍的差别。如果用大文件total(24G)，这个就相对稳定很多（但也偶尔看见50%的差别）。

### 测试结果 random vs sequential in different block size

| mode | bs | Tp | fio command |
| --- | -------- | -------- | --- |
| random | 4KB | 8.9MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=4k --io_size=2G --rw=randread --randrepeat=0 |
| sequential | 4KB | 18.6MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=4k --io_size=3G --rw=read |
| random | 8KB | 16.8MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=8k --io_size=3G --rw=randread --randrepeat=0 |
| sequential | 8KB | 33.5MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=8k --io_size=5G --rw=read |
| random | 16KB | 33.7MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=16k --io_size=5G --rw=randread --randrepeat=0 |
| sequential | 16KB | 62.7MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=16k --io_size=10G --rw=read |
| random | 32KB | 61.4MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=32k --io_size=10G --rw=randread --randrepeat=0 |
| sequential | 32KB |105MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=32k --io_size=15G --rw=read |
| random | 64KB | 110MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=64k --io_size=20G --rw=randread --randrepeat=0 |
| sequential | 64KB | 203MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=64k --io_size=30G --rw=read |
| random | 128KB | 161MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=128k --io_size=40G --rw=randread --randrepeat=0 |
| sequential | 128KB | 230MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=128k --io_size=80G --rw=read |
| random | 256KB | 255MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=256k --io_size=65G --rw=randread  --randrepeat=0 |
| sequential | 256KB | 396MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=256k --io_size=70G --rw=read |
| random | 512KB | 310MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=512k --io_size=70G --rw=randread --randrepeat=0 |
| sequential | 512KB | 393MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=512k --io_size=70G --rw=read |
| random | 1024KB | 643MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=1024k --io_size=70G --rw=randread --randrepeat=0 |
| sequential | 1024KB | 725MB/s | fio --name=t --filename=total --ioengine=sync --direct=1 --bs=1024k --io_size=70G --rw=read |

### 分析

1. random和sequential的对比：同一block size，对于throughput，顺序读比随机读有优势。在block size小时（小于或等于64k），是一倍的关系。在bs > 128k，也是sequential比random要更好一些。感觉上，sequential好像是预先读出（因为SSD内部也有SDRAM的cache），因此加快。

2. 单个模式下block size变化规律：block size越大，则throughput越大，block size小时（小于或等于64k），block size大一倍，throughput也接近一倍。最大block size，i.e., 1024k，相比最小的block size，i.e., 4k，其throughput相比可以70倍的差别，i.e., random或sequential模式下bs=4k vs bs=1024k。 这比较符合SSD的工作原理，即并发导致高速（parallelism for performance），而block size比较高时，利于SSD内部做并发。而block size到了128k以上时，并发的边际效应开始降低。

3. 因为block size和throughput的关系，可以推算出，IOPS在block size比较小的时候，比较高，在k级别。当block size比较高时，IOPS开始降低，比如：bs=1024K下，IOPS在几百。这个和HDD比，是一个数量级。一个HDD，在1024K下，可以到近200。但HDD在其他bs下，也是这个IOPS。所以，SSD的优势在于小的block size。

## random read with multi thread and io depth

### WiscKey里的测试

[对这篇文章 WiscKey- Separating Keys from Values in SSD-conscious Storage， Figure3](https://www.usenix.org/system/files/conference/fast16/fast16-papers-lu.pdf)，里面的东西有所怀疑，因为从图示分析得到下面三点，

1. sequential在block size从小到大时，throughput几乎无变化。有两种可能，其一，就是queued depth很大，这样小的block size即使咋单线程下，也可以因为queue汇合成大的block size；但注意：我的Mac上的SSD没有这个特性，也许其他企业级的SSD如此；其二，就是多线程效果，这样并发的请求达到和同样的效果。NOTE: 对于我的Mac，只有一种情况下可以实现这种sequential下不管block size是多少，都是最高的throughput，即必须启用page cache，i.e., direct = 0。

2. random read随着block size增大，不管是单线程，还是多线程，都是增大的。但多线程，有进一步放大的效果。在一定线程数和不太大的block size下，对于random read，也可以达到和sequential read一样的最高的throughput。图中单线程只到block size=256K这种情况，其throughput也达到最大值的近60%，如果能到block size=1M，怀疑能接近最大的带宽。

3. 多线程的因素，到底是多线程，还是io任务队列足够多（需要做单线程多io任务， 和多线程的比较）

但我自己的验证有些不同，见下表

### 我自己Mac测试情况

我自己在Mac上做上面的测试 （注意：用```for i in {1..5}; do <>; done，因为一次的数据可能会有很大的倍数级别的偏差）

| bs | threads | iodepth | throughtput | command |
| -- | -- | -- |  -- | -- |
| 4k | 1 | 1 | 10.8MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=4k --io_size=1G --rw=randread --iodepth=1 --numjobs=1 --thread --group_reporting --randrepeat=0 |
| 4k | 1 | 32 | 13.1MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=4k --io_size=1G --rw=randread --iodepth=32 --numjobs=1 --thread --group_reporting --randrepeat=0 |
| 4k | 32 | 1 | 13.6MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=4k --io_size=100M --rw=randread --iodepth=1 --numjobs=32 --thread --group_reporting --randrepeat=0 |
| 4k | 32 | 32 | 13.4MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=4k --io_size=100M --rw=randread --iodepth=32 --numjobs=32 --thread --group_reporting --randrepeat=0 |
| 16k | 1 | 1 | 39.7MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=16k --io_size=3G --rw=randread --iodepth=1 --numjobs=1 --thread --group_reporting --randrepeat=0 |
| 16k | 1 | 32 | 46.1MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=16k --io_size=4G --rw=randread --iodepth=32 --numjobs=1 --thread --group_reporting --randrepeat=0 |
| 16k | 32 | 1 | 45.6MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=16k --io_size=400M --rw=randread --iodepth=1 --numjobs=32 --thread --group_reporting --randrepeat=0 |
| 16k | 32 | 32 | 46.6MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=16k --io_size=400M --rw=randread --iodepth=32 --numjobs=32 --thread --group_reporting --randrepeat=0 |
| 64k | 1 | 1 | 134MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=64k --io_size=12G --rw=randread --iodepth=1 --numjobs=1 --thread --group_reporting --randrepeat=0 |
| 64k | 1 | 32 | 166MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=64k --io_size=16G --rw=randread --iodepth=32 --numjobs=1 --thread --group_reporting --randrepeat=0 |
| 64k | 32 | 1 | 164MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=64k --io_size=1G --rw=randread --iodepth=1 --numjobs=32 --thread --group_reporting --randrepeat=0 |
| 64k | 32 | 32 | 175MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=64k --io_size=1G --rw=randread --iodepth=32 --numjobs=32 --thread --group_reporting --randrepeat=0 |
| 256k | 1 | 1 | 277MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=256k --io_size=20G --rw=randread --iodepth=1 --numjobs=1 --thread --group_reporting --randrepeat=0 |
| 256k | 1 | 32 | 380MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=256k --io_size=25G --rw=randread --iodepth=32 --numjobs=1 --thread --group_reporting --randrepeat=0 |
| 256k | 32 | 1 | 432MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=256k --io_size=2G --rw=randread --iodepth=1 --numjobs=32 --thread --group_reporting --randrepeat=0 |
| 256k | 32 | 32 | 401MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=256k --io_size=2G --rw=randread --iodepth=32 --numjobs=32 --thread --group_reporting --randrepeat=0 |
| 1024k | 1 | 1 | 534MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=1024k --io_size=40G --rw=randread --iodepth=1 --numjobs=1 --thread --group_reporting --randrepeat=0 |
| 1024k | 1 | 32 | 762MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=1024k --io_size=50G --rw=randread --iodepth=32 --numjobs=1 --thread --group_reporting --randrepeat=0 |
| 1024k | 32 | 1 | 836MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=1024k --io_size=4G --rw=randread --iodepth=1 --numjobs=32 --thread --group_reporting --randrepeat=0 |
| 1024k | 32 | 32 | 775MB/s | fio --name=t --filename=total --ioengine=libaio --direct=1 --bs=1024k --io_size=4G --rw=randread --iodepth=32 --numjobs=32 --thread --group_reporting --randrepeat=0 |

结论:

1. 当threads=1时，iodetpht=1, 这里所用的libaio，和上面的sync random read比，差别不大。

2. 起决定性作用的，还是block size（倍数级增加）。然后多thread或多iodepth，有加强（50%以内）。

3. 以上都是randomread，但在block size = 1024k时，不管是iodepth，还是多线程，其性能都可以和最上面的单线程同步顺序(sequential)的数据一个水准。

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
1. --end_fsync=1，最后文件写完，保证给一个fsync，因为缺省下fsync=0

2. 如果direct=1，那么fsync参数不保证有效，[参考fio的文档](https://fio.readthedocs.io/en/latest/fio_doc.html#)。对于写，我们基本不考虑direct=1，即我们一定要用到OS page cache.

3. 如果测试时间比较短（只有几十秒），请用多次```for i in {1..5}; do <command>; done```，然后取中间值的throughput作为其代表

我们主要测试，不同block size下，fsync是0（不发出)，1（相当于每次都sync）或其他值（相当于batch/group commit）的情况

### 测试结果

NOTE: loop测试写前，重新创建文件(所以下面用rm命令)。如果下一个仍用上一个文件，会导致一些数据失真。

#### fsync = 0，sequetial write

| bs | throughput | fio command |
| -- | -- | -- |
| 4k | 191MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=20G --bs=4k; |
| 8k | 193MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=20G --bs=8k; | 
| 16k | 196MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=20G --bs=16k; | 
| 32k | 187MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=20G --bs=32k; | 
| 64k | 193MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=20G --bs=64k; | 
| 128k | 194MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=20G --bs=128k; | 
| 256k | 191MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=20G --bs=256k; | 
| 512k | 191MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=20G --bs=512k; | 
| 1024k | 191MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=20G --bs=1024k; |

#### fsync = 1，sequential write

| bs | throughput | fio command |
| -- | -- | -- |
| 4k | 2.6MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=1 --end_fsync=1 --size=200M --bs=4k; |
| 8k | 5.7MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=1 --end_fsync=1 --size=300M --bs=8k; | 
| 16k | 10.2MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=1 --end_fsync=1 --size=500M --bs=16k; | 
| 32k | 21.6MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=1 --end_fsync=1 --size=1G --bs=32k; | 
| 64k | 44.9MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=1 --end_fsync=1 --size=2G --bs=64k; | 
| 128k | 76.4MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=1 --end_fsync=1 --size=4G --bs=128k; | 
| 256k | 133MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=1 --end_fsync=1 --size=8G --bs=256k; | 
| 512k | 175MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=1 --end_fsync=1 --size=15G --bs=512k; | 
| 1024k | 191MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=1 --end_fsync=1 --size=17G --bs=1024k; |

#### block = 4k，fsync = 1, 2, 4, 8, 16, 32, 64, 128，sequential write

| fsync | throughput | fio command |
| -- | -- | -- |
| 1 | 2.8MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=1 --end_fsync=1 --size=200M --bs=4k; |
| 2 | 5.1MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=2 --end_fsync=1 --size=400M --bs=4k; |
| 4 | 12.9MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=4 --end_fsync=1 --size=700M --bs=4k; |
| 8 | 23.7MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=8 --end_fsync=1 --size=1500M --bs=4k; |
| 16 | 35.6MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=16 --end_fsync=1 --size=3G --bs=4k; |
| 32 | 81.9MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=32 --end_fsync=1 --size=5G --bs=4k; |
| 64 | 120MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=64 --end_fsync=1 --size=8G --bs=4k; |
| 128 | 168MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=128 --end_fsync=1 --size=10G --bs=4k; |

#### block = 32k，fysnc = 1, 2, 4, 8, 16, 32, 64, 128，sequential write

| fsync | throughput | fio command |
| -- | -- | -- |
| 1 | 19.9MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=1 --end_fsync=1 --size=1G --bs=32k; |
| 2 | 42.9MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=2 --end_fsync=1 --size=2G --bs=32k; |
| 4 | 77.5MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=4 --end_fsync=1 --size=4G --bs=32k; |
| 8 | 110MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=8 --end_fsync=1 --size=7G --bs=32k; |
| 16 | 177MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=16 --end_fsync=1 --size=10G --bs=32k; |
| 32 | 188MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=32 --end_fsync=1 --size=15G --bs=32k; |
| 64 | 221MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=64 --end_fsync=1 --size=15G --bs=32k; |
| 128 | 198MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --fsync=128 --end_fsync=1 --size=15G --bs=32k; |

#### randwrite with direct=0 and fsync=0

| bs | throughtput | fio command |
| -- | -- | -- |
| 4k | 15.3MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=22G --io_siz=1G --bs=4k; |
| 8k | 29.3MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=22G --io_siz=2G --bs=8k; |
| 16k | 47.6MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=22G --io_siz=3G --bs=16k; |
| 32k | 69.0MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=22G --io_siz=4G --bs=32k; |
| 64k | 96.3MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=22G --io_siz=5G --bs=64k; |
| 128k | 104MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=22G --io_siz=5G --bs=128k; |
| 256k | 104MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=22G --io_siz=5G --bs=256k; |
| 512k | 110MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=22G --io_siz=5G --bs=512k; |
| 1024k | 101MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=22G --io_siz=5G --bs=1024k; |

注意：randwrite下，如果io_size接近或超过size，会导致数据失真，如下表的测试：

| size | io_size | throuhgput | fio command |
| -- | -- | -- | -- |
| 1G | 100M | 17.7MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=1G --io_siz=100M --bs=4k; |
| 1G | 200M | 26.9MB/s |  rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=1G --io_siz=200M --bs=4k; |
| 1G | 400M | 27.8MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=1G --io_siz=400M --bs=4k; |
| 1G | 800M | 65.2MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=1G --io_siz=800M --bs=4k; |
| 1G | 2G | 62.6MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=1G --io_siz=2G --bs=4k; |
| 1G | 4G | 78.3MB/s | rm -f w.0.0; fio --name=w --rw=randwrite --randrepeat=0 --ioengine=sync --direct=0 --fsync=0 --end_fsync=1 --size=1G --io_siz=4G --bs=4k; |

失真的原因怀疑是：如果io_size过大，那么重复到同一block的机会加大，导致throughput有偏差。

#### 分析

1. 对于sequential write, 当fysnc=0时，bs从4k到1024k，throughput都差别不大，都是200M左右。这意味写盘都先到page cache里，然后由os来write back。一般而言，都是接近磁盘的写的最大带宽。

2. 对于random write，当fsync为0而且direct为0(write back by OS page cache)时，并没有显示出如sequential那样的throughput，是个非常有趣的现象。我的推论是：当sequential write时，在page cache里小的block size（例如：4k）可以组成大的block size，然后写入到disk。而且这些block size为1024k的写入还是连续的，i.e., 可以组成更大的block size。而random write，做不到这一点（因为随机分散到22G大文件的各个部分）。只能batch/group提交。如果os cache或block driver对于内部group有一定限额的话，那么并发的性能并不高。即使random write下block size为1024k，其性能和block size = 4k但sequential write相比，也只能达到后者的一半性能。所以，block size是首个关键（连续4k的block size可以相当于1024k），然后是这些大的block size是否也临近（即理论上能产生更大的block size）。这个对于B树的heap文件写有很大的参考意义。同时，可以参考下面的libaio的测试数据来印证这一推论。

3. 当fsync=1时，是最慢的写盘操作。每一个bs写盘，都要sync到SSD后才能继续。这相当于数据库系统里的每次写盘都sync的配置。是数据最安全的，但也是最慢的。其中，在4k，8k, 16k时，相比fsync=0或fsync!=0但bs=1024k的写盘，有几十倍甚至近百倍的差别。很多数据库的页的大小，或者最小写盘单位，就是这三个单位。

4. 当fsync=1时，当bs比较大，比如512k, 1024k时，其写盘速度和最大带宽差别不大，因为当bs比较大时，SSD的并发优势将会被充分利用到。同时，这也给设计带来一个技巧，就是需要sync=1，可以收集一批小的而且相邻的写，然后集中后用一个比较大的block size写入，这时，其性能基本是最大值，和OS page cache的效果差不多。

5. 通过block size为4k和32k，但fsync的变化可以看出，如果一次fsync能多提交一些请求，即使block size很小，也能达到很大的写入速度。这就是batch/group的效用。

## 多线程和io depth下的write

由于是RocksDB，我们只考虑block szie = 1024k的情况 (以下做5次，取中间值)

### 有误导的测试

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

以上数据惊人，throughput甚至达到了GB/s级别，而且全部和thread数量增加相关。有个怀疑是：其实多线程写，是多线程操作SSD内部的SDRAM，即第一个线程顺序写文件，留下了很多cache在SSD内部的SDRAM里，后面的线程利用了这个cache，虽然还需要再写一遍，但由于是同一文件同一位置，所以速度可以大大提高（甚至可以优化成零写，比如让SSD内部的两个逻辑地址LBA指向同一物理page）。

### 修正的测试

所以，做个修正，让每个线程写不同的文件(i.e. 去掉--filename=)，然后并发线程，看效果如何，见下表

| threads | io depth | throughtput | command |
| -- | -- | -- | -- |
| 4 | 1 | 174MB/s | rm -f w.?.0; fio --name=w  --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=5G --io_size=5G --bs=1024k --iodepth=1 --numjobs=4 --thread --group_reporting; |
| 4 | 4 | 190MB/s | rm -f w.?.0; fio --name=w  --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=4G --io_size=4G --bs=1024k --iodepth=4 --numjobs=4 --thread --group_reporting; |
| 8 | 1 | 189MB/s | rm -f w.?.0; fio --name=w  --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=2G --io_size=2G --bs=1024k --iodepth=1 --numjobs=8 --thread --group_reporting; |
| 8 | 4 | 195MB/s | rm -f w.?.0; fio --name=w  --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=1800M --io_size=1800M --bs=1024k --iodepth=4 --numjobs=8 --thread --group_reporting; |
| 16 | 1 | 187MB/s | rm -f w.?.0; fio --name=w  --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=1500M --io_size=1500M --bs=1024k --iodepth=1 --numjobs=16 --thread --group_reporting; |

我们发现当用多个文件，让每个线程都独立处理自己的文件时，写入的速度没有那么快（i.e.，到夸张的GB/s），还是普通的200M上下。这才是SSD真实的写的最高性能（block size很大，同时还有多线程，iodepth也可以有多个）。

### 附一：block size = 1024k, random write

补充：我们看一下block size是1M，但是random write的情况：

| threads | io depth | throughtput | command |
| -- | -- | -- | -- |
| 1 | 1 | 102MB/s | rm -f w.?.0; fio --name=w --rw=randwrite --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=22G --io_size=5G --bs=1024k --iodepth=1 --numjobs=1 --thread --group_reporting --randrepeat=0; |
| 1 | 4 | 108MB/s | rm -f w.?.0; fio --name=w --rw=randwrite --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=22G --io_size=5G --bs=1024k --iodepth=4 --numjobs=1 --thread --group_reporting --randrepeat=0; |
| 4 | 1 | 116MB/s | rm -f w.?.0; fio --name=w --rw=randwrite --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=6G --io_size=1G --bs=1024k --iodepth=1 --numjobs=4 --thread --group_reporting --randrepeat=0; |
| 4 | 4 | 110MB/s | rm -f w.?.0; fio --name=w --rw=randwrite --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=6G --io_size=1G --bs=1024k --iodepth=4 --numjobs=4 --thread --group_reporting --randrepeat=0; |
| 8 | 1 | 102MB/s | rm -f w.?.0; fio --name=w --rw=randwrite --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=3G --io_size=600M --bs=1024k --iodepth=1 --numjobs=8 --thread --group_reporting --randrepeat=0; |
| 8 | 4 | 112MB/s | rm -f w.?.0; fio --name=w --rw=randwrite --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=3G --io_size=600M --bs=1024k --iodepth=4 --numjobs=8 --thread --group_reporting --randrepeat=0; |

### 附二：block size = 4k, single thread and sequential

只考虑block size=4k，同时只是最后用sync，i.e., fsync=0 and end_fsync=1。

| bs | iodepth | Tp | fio command |
| :-: | :-: | -- | -- |
| 4k | 1 | 26.0MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=2G --bs=4k --iodepth=1; |
| 4k | 2 | 32.1MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=2G --bs=4k --iodepth=2; |
| 4k | 4 | 56.6MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=4G --bs=4k --iodepth=4; |
| 4k | 8 | 56.7MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=7G --bs=4k --iodepth=8; |
| 4k | 16 | 55.9MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=7G --bs=4k --iodepth=16; |
| 4k | 32 | 56.9MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=7G --bs=4k --iodepth=32; |
| 4k | 512 | 62.7MB/s | rm -f w.0.0; fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --size=7G --bs=4k --iodepth=512; |

1. 在我的机器上，iodepth到了4，基本就是libaio就到了最大速度
2. 和OS page cache（engine=sync with direct = 0）相比，速度差的很远。说明libaio对于buffer的优化不如OS。

### 附三：block size = 4k, multi thread and sequential but multi files

| bs | threads | Tp | fio command |
| -- | -- | -- | -- |
| 4k | 1 | 51.7MB/s | rm -f w.?.0; fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --bs=4k --iodepth=4 --size=2G --numjobs=1 --thread --group_reporting; |
| 4k | 2 | 52.7MB/s | rm -f w.?.0; fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --bs=4k --iodepth=4 --size=2G --numjobs=2 --thread --group_reporting; |
| 4k | 4 | 50.2MB/s | rm -f w.?.0; fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --bs=4k --iodepth=4 --size=2G --numjobs=4 --thread --group_reporting; |
| 4k | 8 | 45.5MB/s | rm -f w.?.0; fio --name=w --rw=write --ioengine=libaio --direct=1 --end_fsync=1 --fsync=0 --bs=4k --iodepth=4 --size=2G --numjobs=8 --thread --group_reporting; |

1. 可以看到，多线程作用不大。和上面的AIO（见“修正的测试”）达到最高速相比较，block size才是关键。

# write mix with Read 

## write of page cache with random 4k read

我们测试下面这种情况：首先write是log模式，同时全部走page cache，这样write log是最大效率。同时，另外一个进程（或线程）同时并发bs=4k的random read。然后看互相的影响。

在没有写的影响，纯粹的随机读，参考上面的测试数据，throughput是10M/s左右，i.e.，IOPS是2K以上。

在没有读的影响下，纯粹的顺序走page cache写，参考上面的测试数据，throughput是200MB/s左右。

然后，我们尝试下面的命令
先启动随机读， bs=4k, 不走cache(direct=1)
NOTE: totoal是上面的网上一个2.6G的安装包，然后cat后，形成10G左右的一个文件。
```
for i in {1..50}; do fio --name=r --filename=total --ioengine=sync --rw=randread --io_size=10M --bs=4k --direct=1 --randrepeat=0; done
```
紧跟着马上几乎同时启动顺序写，bs=1024k，走page cache, i.e., direct=0 & fsync=0
```
for i in {1..20}; do rm w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=12G --fsync=0 --bs=1024k; done
```

我们发现，对于顺序走page cache的写，其throughput变化不太大，基本还是200MB/s上下。

但对于随机读，就付出了代价。没有写时，是10MB/s。当加了写后，降低到100-200K/s，近100倍的降低。

## write of page cache with random 1024k read

这个是模拟log写，和background做compaction时的随机大block size读的情况

Mix中写的命令
```
for i in {1..20}; do rm w.0.0; fio --name=w --rw=write --ioengine=sync --direct=0 --end_fsync=1 --size=12G --fsync=0 --bs=1024k; done
```
然后几乎同时启动随机block size=1024k的读(顺序读按理会更模拟真实，但在bs=1024k后，随机读和顺序读差别不大，同时防止顺序读被SSD内部优化)
```
for i in {1..10}; do fio --name=r --filename=total --ioengine=sync --rw=randread --io_size=2G --bs=1024k --direct=1 --randrepeat=0; done
```

如果读写都是单独的，从上面的测试知道，write的throughput到200MB/s，而read到600MB/s以上。

我们发现，对于写，没有太大影响， 比上面的block size=4k影响还要小。

如果用上面，

先写再启动读，则写的throughput仍然是200MB/s，而读则降低到20MB/s，即30倍的降幅。

先读再启动写，测试结果类似，写变化不大，但读下降很多。

## 分析和结论

所以，通过两个测试，可以知道，对于经过page cache的log写（block size=1024k），其throughput没有严重的影响。影响大的是读（我们只考虑随机读，这也是生产环境的真实情况），不管是block size = 4k，还是block size = 1024k，和纯粹的比，都有大幅的至少几十倍的降低。

不过，好在，读我们是很容易做load balance的，所以写不受太大影响，是好事。

真正对于写的优化是：log写有两个，一个是注入ingest写，一个是background compaction的写。如何分配并发，同时如何降低write amplification，才是写优化的两个关键。

