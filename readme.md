
# 小心假数据

我曾经做过一个项目，使用SSD做内存的辅助，提升一个Cache系统的性价比。我浏览网上一些讯息，看上去，SSD的性能是十分惊人的。比如Throughput，一个普通的NVMe的SSD，动不动就几千兆Byte/s，少则也有500MB/s。而IOPS，也是几百k/s的级别。

而HDD，如果不考虑RAID，单纯一个磁盘，它的Throughput就是百MB/s，更糟糕的是IOPS，每秒只有百来个。HDD读写有一个很大的特性，就是一个随机random读写，相对于顺序sequential读写，差别是一个天文数字。比如：[最新的数据](https://colin-scott.github.io/personal_website/research/interactive_latency.html)，sequential读1MB，不到1ms，而随机读一次，需要2ms。假设一个随机读是1KB的话，那么一个Sequential读写，相当于2000个random读写。

再看HDD IOPS的制约，假设一个随机读写只有16KB（MySQL的一个页面的缺省大小），那么实际的吞吐量就不到2MB/s，是理论最大带宽的2%左右。

这也是为什么，在数据库设计里，大量使用日志LOG，因为log就是sequential读写。

比如：MySQL里，对于事务，先对磁盘做redo & undo Log，真正的page的改写，先在内存page里做，暂不写到对应的磁盘page上。万一发生掉电或程序crash的意外，因为有redo log在磁盘上，我们可以在数据库重启时，通过redo log重做一次，这样就保证了durability的承诺。但这是一个tradeoff，因为重做redo是一个慢操作（因为我们要重头读取，而且要针对所有修改一次性全部做一遍）。但这是值得的，因为毕竟数据库发生故障的概率是小事件。

同时，也导致另外一个特性checkpoint。

内存暂被修改但未刷到磁盘的page，我们称之为dirty page，会越来越多，一是内存可能容不下这么多的dirty page，二是如果dirty page过多，万一发生crash后，重启时重写的页面会非常多，如果这个时间太大（比如小时级），也是实际业务不可接受的。所以，在某些时间点上，i.e. checkpoint，我们必须需将这些dirty page部分或全部刷新(flush)到磁盘上。你可能发现，这还是随机写，好像并没有节省磁盘读写。但实际上，有很多优化：
1. 实际数据库请求不可能是滔滔不绝的，在请求不忙的时刻，可以做checkpoint；
2. 如果一个页面在checkpoint之间，被修改多次，那么只要一次写盘就够了，是一个合并写；
3. 更奇妙的，如果几个被修改的页面连续或接近，我们可以将多次random写，变成一次sequential写。

即使log这个顺序写，也可以利用HDD磁盘特性。因为log虽然是堆积append only和顺序sequential，但毕竟每秒都产生1MB的log机会是不多的。所以log也可以先写到OS的缓存，然后每秒flush一次，这样，几次小的事务log会形成一个大的磁盘写。

这也是很多数据库，都提供一个配置选项，可以设置每个事务必须一次flush(sysnc/per transaction)，还是每秒定期一个flush(sync/per second)。我在《MySQL技术内幕》这本书上看到，有人测试过，这个每秒 flush log 选项，相比可以带来10倍的性能提升。但这又是一个tradeoff，因为本来我们用redo log，就是防止数据丢失。但如果设置了每秒flsh log一次，我们就有丢失1秒数据的风险。

但实际生产(production)环境下，因为有这个10倍的好处，对于很多WEB应用，都是建议用每秒刷新log的。除非到了金融级别，才真正考虑每次交易都刷盘flush的策略。

所以，这也是我们在设计数据库的一个考量：可以丢失一部分数据，只要概率比较低，同时万一丢失，损失量可控（比如：1秒）。

再回到SSD，如果SSD的IOPS和Throughput，相对于HDD，有这么巨大的提升，那我们的数据库性能，不是可以什么都不做的情况下，最少十倍，甚至千倍的提升？

但我的实践发现不是如此。[这也就是我为什么在RedRock项目里有感而发。](https://github.com/szstonelee/redrock/blob/master/documents/performance_en.md)

关键是：
1. SSD Throughput没有那么高，在一些场景下，和HDD比起来，优势不大
2. SSD IOPS确实比HDD高很多，但视乎情况depends

所以，我们需要了解SSD的特性，即在各种环境下的Throughput和IOPS的数据。

# dd tool in Linux，一个简单的磁盘工具

## 说明

[参考网上dd这个说明。](https://linuxaria.com/pills/how-to-properly-use-dd-on-linux-to-benchmark-the-write-speed-of-your-disk)

我们使用oflag=sync这个参数，因为这样每个block，都会data sync到文件，同时sync到file meta(比如：更新文件最新时间等)。否则，缺省下，没有sync，就会出现：
1. 磁盘写，其实是内存写，测试的throughput，其实是内存的带宽(主存Main Memory1秒跑1G肯定没有问题，上限是10G)
2. conv=fsync，这个是一次性sync，比如：count=1000，发出了1000次写，先到内存，然后一次性flush到磁盘，也不真实

这也是我们不能在MacOS下做这个操作，因为MacOS的dd不支持oflag。

## 测试结果

我的机器上的结果(NVMe, 120G, Ubuntu 18.04 VM in MacOS)

| Block Size | Throughput(Bandwidth) | Command |
|:----------:|:----------:|--------:|
| 1K | 1.3 MB/s | dd bs=1K count=100000 if=/dev/zero of=test oflag=sync |
| 4K | 5.2 MB/s | dd bs=4K count=50000 if=/dev/zero of=test oflag=sync |
| 16K | 20.1 MB/s | dd bs=16K count=10000 if=/dev/zero of=test oflag=sync |
| 64K | 76.3 MB/s | dd bs=64K count=10000 if=/dev/zero of=test oflag=sync |
| 128K | 120 MB/s | dd bs=128K count=10000 if=/dev/zero of=test oflag=sync |
| 512K | 200 MB/s | dd bs=512K count=5000 if=/dev/zero of=test oflag=sync |
| 1M | 262 MB/s | dd bs=1M count=1000 if=/dev/zero of=test oflag=sync |
| 2M | 371 MB/s | dd bs=2M count=1000 if=/dev/zero of=test oflag=sync |
| 4M | 226 MB/s | dd bs=4M count=1000 if=/dev/zero of=test oflag=sync |

## 分析

1. Block Size比较小时，Throughput比较小，只有几兆，是最大带宽值的百分之一不到
2. 随着Block Size增加，Throughtput也增加，到Block Size==128K前，接近线性
3. 128K开始，有增加，但增幅减少
4. 并不是BlockSize越大，Thourghput就越大，比如Block Size==4M下，比2M下还要小

设想: 对于基本都是小值的key/value，比如Redis下OLTP应用，存储数据库的查询所产生的实际数据吞吐量，很可能是个很小的值（几个MB/s）。

## dd的缺陷

但dd有以下缺陷：
1. 单线程测试
2. 其实是sequential写（即使1K也是，不过每次都sync会相对比较仿真离散）
3. 只能测试写
4. 没有IOPS

# fio tool in Linux，更准确的一个磁盘工具

## fio说明

[参考网上资料](https://wiki.mikejung.biz/Benchmarking#Fio_Test_Options_and_Examples)

我们的设置：

--direct=1，这样，OS的buffer就不起作用

--fsync=0，如果为1，每个block都flush再做下一个。但那样和实际运行不一致，就如上面的数据库flush策略范例

fsync == 0 or == 1的比较

尝试和1做过比较，差别不是太大，稍微有提高。只有sequential write(且bs=4k)时，有5或6倍的巨大差别，怀疑是seqential write时，driver或SSD做了写合并。只有1:bs小时;2:sequential条件下，合并才有效。但奇怪的是sequential read没有什么影响。

然后，我们测试不用的场景，包括：
1. blocksize，bs=4K, 16K, 128K, 1M 
2. numjobs，多少个file，测试包括1个， 4个, 总大小都是4G。可以理解为多少个thread并发 
3. rw，包括randread， randwrite，randrw(r/w：3:1)， read（sequential), write(sequential)

### Random read下fio命令
```
fio --name=test --rw=randread --bs=4K --direct=1 --numjobs=1 --size=4G --runtime=120 --group_reporting 
fio --name=test --rw=randread --bs=4K --direct=1 --numjobs=4 --size=1G --runtime=120 --group_reporting 
fio --name=test --rw=randread --bs=1M --direct=1 --numjobs=1 --size=4G --runtime=120 --group_reporting 
```
### Random write下fio命令
fio --name=test --rw=randwrite --bs=4K --direct=1 --numjobs=1 --size=4G --runtime=120 --group_reporting
### Random read/write, read=75% (r/w 3:1)下fio命令
fio --name=test --rw=randrw --rwmixread=75 --bs=4K --direct=1 --numjobs=4 --size=1G --runtime=120 --group_reporting
### Sequential read下fio命令
fio --name=test --rw=read --bs=4K --direct=1 --numjobs=4 --size=1G --runtime=120 --group_reporting
### Sequential write下fio命令
fio --name=test --rw=write --bs=4K --direct=1 --numjobs=4 --size=1G --runtime=120 --group_reporting

## 测试结果

| block size | job number	| mode	| read iops	| read bandwidth	| write iops | write bandwidth |
|:-----------|:-----------|:------|----------:|--------:|-----------:|---------:|
| bs=4K |	numjobs=1 |randread         | 2855 | 11.7MB/s |	
|       |           |randwrite        |      |          | 3109 | 12.7MB/s |	
|	      |	          |randrw(3:1)      |	1949 | 7986kB/s |  652 | 2673kB/s |  
|		    |           |read sequential  |	4429 | 18.1MB/s	|      |          | 
|		    |           |write sequential	|	     |          | 7266 | 29.8MB/s | 
|       |	numjobs=4	|randread	        | 3941 | 16.1MB/s	|      
|       |           |randwrite        |      |          | 4341 | 17.8MB/s | 
|       |		        |randrw(3:1)	    | 2957 | 12.1MB/s	| 991	 | 4060kB/s |
|		    |           |read sequential	| 3734 | 15.3MB/s	|	
|		    |           |write sequential |	     |          | 12.2k| 49.0MB/s |
|bs=16K	| numjobs=1	|randread         |	2490 | 40.8MB/s	|	
|       |           |randwrite        |      |          | 3754 | 61.5MB/s |
|		    |           |randrw(3:1)      |	1782 | 29.2MB/s	| 595  | 9758kB/s |
|		    |           |read sequential	| 4442 | 72.8MB/s	|	
|       |           |write sequential	|	     |          | 6236 | 102MB/s  |
|       | numjobs=4	|randread         |	2885 | 47.3MB/s |		
|       |           |randwrite        |      |          | 4832 | 79.2MB/s |     
|	      |           |randrw(3:1)	    | 2692 | 44.1MB/s	| 902	 | 14.8MB/s |
|       |       		|read sequential	| 3045 | 49.9MB/s |		
|		    |           |write sequential	|		   |          | 9806 | 161MB/s  |
|bs=128K|	numjobs=1	|randread	        | 1622 | 213MB/s  |
|       |           |randwrite        |      |    		  | 1730 | 227MB/s  |
|		    |           |randrw(3:1)	    | 3297 | 432MB/s	| 1097 | 144MB/s  |
|		    |           |read sequential	| 5419 | 710MB/s  |		
|		    |           |write sequential	|      |          | 1860 | 244MB/s  |
|	      | nunjobs=4	|randread         |	1786 | 234MB/s  |	
|       |           |randwrite        |      |          |	2008 | 263MB/s  |
|		    |           |randrw(3:1)	    | 1270 | 167MB/s  | 437  | 57.3MB/s |
|		    |           |read sequential	| 2289 | 300MB/s	|	
|		    |           |write sequential	|      |          | 2618 | 343MB/s  |
|bs=1M	| numjobs=1	|randread	        | 234	 | 246MB/s 	|	
|       |           |randwrite        |      |          | 239  | 251MB/s  | 
|		    |           |randrw(3:1)	    | 438  | 460MB/s  | 151 | 158MB/s   |
|		    |           |read sequential	| 645	 | 677MB/s  |		
|		    |           |write sequential	|	     |          | 262	| 275MB/s   |
|	      | numjobs=4 |randread	        | 538	 | 564MB/s	|	
|       |           |randwrite        |      |          | 231 | 243MB/s   | 
|		    |           |randrw(3:1)      | 234	 | 246MB/s  | 83  |	87.5MB/s  |
|		    |           |read sequential	| 488	 | 512MB/s  |		
|		    |           |write sequential	|      |          |	921 | 966MB/s   |


## 和Rocksdb针对SSD测试的对比

[RocksDB对于SSD也有个测试]（https://github.com/facebook/rocksdb/wiki/Performance-Benchmarks#fio-test-results）

从它的结果看，在direct=1下，bs=4K，randread，BW(bandwith)500MB/s上下， iops也到了100K以上

但我用类似的fio命令测试（对于第一个fio，我只能做8个job，用8G空间，但这个不应该是决定性因素，第二个fio命令是一样的），发现Throughput还是10MB/s多一点，iops还是不到3k。和我自己上面的测试报告类似。

可能有几个东西影响：

1. 操作系统，我的是Linux VM in MacOS
2. 我的SSD太烂

但我没有更多的机器和其他的SSD进行类比测试，所以，这对于我是个很大的疑惑（我相信Facebook的测试决定是真实的，但我上面的数据也是实测的）

## 分析

1. 一般而言block size越大，带宽Throughtput也越大，和dd结果一致
2. 但增大不是线性增加的，增幅在递减。而当block size过大时，反而降低，这个和dd的结果是一致的
3. 在不用到buffer情况下（direct=1），基本很难达到10K IOPS，这也是真实的SSD性能
4. 用buffer情况下，有几十几百K的IOPS（不在报表里体现，自己可以设置direct=0测试一下），不过，那应该算是幻象，是内存高速带宽的原因
5. 随着block size变大，iops在减少。即每个请求数据变大，吞吐Throughtput上去了，但iops下来了
6. iops几K是常态，超过10k很难。大block size下，比如MB时，低于1K
7. 和dd的数据对比，同样的block size下，Throughput要高，原因这应该是：dd每次io都要sync，而fio不用（fsync=0）
8. Sequential同样block size下，所对应的Throughput要高，这也是合理的。这说明SSD有和HDD类似的特性，只是没有HDD千倍那么夸张。SSD也就2-3倍
9. 并发情况下，Throughput稍微高点，但不明显，可以忽略
10. 在高带宽下，除个别参数外，大部分也就是200MB/s或300MB/s。相当于HDD没有太大优势。所以在OLAP业务中，使用HDD性价比会更好

*真正想利用SSD超越HDD的着力点，是小block size下10K iops的表现，这个相对于HDD有百倍的优势。也是OLTP可以大有可为的痛点。但如果想在MB/s这个Throughput指标下做文章，SSD并没有太大优势。*

具体到RocksDB，到数据库里去查key，在SSD上应该有很大优势。HDD一秒只能做百次的disk seek，到了SSD那里，可以到每秒万次。但RocksDB做Compaction时，都是大Block Size操作（一般RocksDB下一个sst文件是几M），这时，虽然SSD带宽到了百兆，但实际和HDD比起来，并不突出。

还有注意：RocksDB去查key，并不能做到一次获得，需要到多个文件了查询，理论上比B+ tree要多（对于B+ Tree, 如果branch factor=100的话，1 Million的记录，需要4次disk seek才可以查到）。

 有两个没有搞清楚的地方，

 其一，write的throughput会略高于read，这个很奇怪，因为按理write会导致SSD内部的gc起效，从而降低throughput；[可以参考这篇文章](https://www.enterprisestorageforum.com/storage-hardware/ssd-vs-hdd-speed.html)

 其二，是某些参数下的throughput会特别不符合规律，比如128K下那个sequetial read。