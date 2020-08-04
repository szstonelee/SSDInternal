
# 小心上当

我曾经做过一个项目，使用SSD做内存的辅助，提升一个Cache系统的性价比。我发现网上一些数据看上去，SSD的性能是十分惊人的。比如Throughput，一个普通的NVMe的SSD，动不动就几千兆Byte/s，少则也有500MB/s。而IOPS，也是几百K/s的级别。

相比较硬盘HDD，如果不用RAID单纯考虑独立一个磁盘，它的Throughput就是MByte/s，更糟糕的是IOPS，只有百/s这个级别。所以HDD读写有一个很大的特性，就是一个随机random读写，相对于顺序sequential读写，差别是一个天文数字。比如：[最新的数据](https://colin-scott.github.io/personal_website/research/interactive_latency.html)，sequential读1MB，不到1ms，而随机读一次，需要2ms。假设一个随机读是1KB的话，那么一个Sequential读写，相当于2000个random读写。如果再考虑IOPS的制约，假设一个随机读写只有16KB（MySQL的一个页面的缺省大小），那么实际的吞吐量就不到2MB/s，是最大带宽利用率的2%。

这也是为什么，在数据库设计里，大量使用日志LOG，因为log就是sequential读写。

比如：MySQL里，对于事务，先对磁盘做redo & undo Log，真正的Page的改写，先在内存里做，暂不写到对应的磁盘page上。万一发生掉电或程序crash的意外，因为有redo log在磁盘上，所以即使内存里真正的Page没有刷（flush）到磁盘，我们也可以在数据库重启时，通过redo log重做一次对未修改的实际磁盘page的修改，这样就保证了durability的承诺。但这是一个tradeoff，因为重读redo，并apply那些对应的page修改，是一个慢操作（因为我们要重头读取，而且要针对所有一次性全部apply一遍）。但这是值得的，因为毕竟数据库发生故障的概率是小事件，而利用sequential/random这个便宜是每个时刻都可以用到的。

同时，也导致另外一个技术特性checkpoint，因为内存的暂被修改但未刷到磁盘的page（我们称之未dirty page）会越来越多，一是内存可能容不下这么多的dirty page，二是如果dirty page过多，万一发生crash，重启恢复重写的页面会非常多，如果这个时间太大（比如小时级），也是实际业务不可接受的。所以，在某些时间点上(也就是checkpoint)，我们必须需将这些dirty page部分或全部刷新(flush)到磁盘上。你可能说，这不还是随机写，并没有节省磁盘读写。但实际上，有很多优化：1. 因为实际数据库请求不可能是持续不断的，所以，在请求不忙的时候，我们可以做checkpoint，因为这时的磁盘是空闲的；2. 如果一个页面在checkpoint之间，被修改多次，那么只要一次写盘就够了，是一个节省；3.更奇妙的，如果几个被修改的页面连续或接近，我们可以将几次random写，变成一次sequential写。

即使log这个顺序写，也可以利用这个磁盘特性。因为log虽然是堆积和顺序，但毕竟每秒都产生超过1MB的log是不多的。所以log也可以先写到OS的缓存，然后每秒flush一次，这样，几次小的事务log会形成一个大的磁盘写。这也是很多数据库，都提供一个配置选项，可以设置每个事务必须一次flush，还是每秒定期一个flush。我在《MySQL技术内幕》这本书上看到，有人测试，这个每秒flush log的选项，可以带来10倍的性能提升。但这又是一个tradeoff，因为本来我们用redo log，就是防止数据丢失。但如果设置了每秒flsh log一次，我们就有一个丢失1秒数据的风险。

但实际生产(production)环境下，因为有这个10倍的好处，对于很多WEB应用，都是建议用每秒刷新log的。除非到了金融级别，才真正考虑每次交易都刷盘flush的策略。

所以，这也是我们在设计数据库的一个考量：可以丢失一部分数据，只要概率比较低，同时万一丢失，损失量可控（比如：1秒）。

再回到SSD，如果SSD的IOPS和Throughput，相对于HDD，有这么巨大的提升，那我们的数据库的性能，不是可以什么都不做的情况下，最少十倍，甚至千倍的提升？

但我的实践发现不是如此。

关键是：
1. Throughput没有那么高，和HDD比起来，一般情况下差不多。
2. IOPS确实比HDD高很多，但depends

# dd tool in Linux

## 说明

[参考网上dd这个说明](https://linuxaria.com/pills/how-to-properly-use-dd-on-linux-to-benchmark-the-write-speed-of-your-disk)

我们使用oflag=sync这个参数，因为这样每个block，都会sync数据到文件，同时sync数据到file meta(比如：更新文件最新的更新时间等)，这个会比较真实的反应SSD的情况。否则，缺省下，就会出现：
1. 没有sync，即你的磁盘写，其实是写到内存里，那么测试的throughput，其实是内存的速度
2. conv=fsync，这个是一次性sync，比如：count=1000，发出了1000次写，先到内存，然后一次性flush到磁盘，也不真实

这也是我们不能在MacOS下做这个操作，因为MacOS的dd不支持oflag

## 测试结果

我的机器上的结果(NVMe, 120G, Ubuntu 18.04 VM in MacOS)
| Block Size | Throughput | Command |
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

1. Block Size比较小时，Throughput比较小
2. 随着Block Size增加，Throughtput也增加，到512K前，接近线性 
3. 并不是BlockSize越大，Thourghput就越大，比如Block Size==4M下，比2M要小

## dd的缺陷

但dd有以下缺陷：
1. 单线程读写
2. 其实是sequential写（即使1K，不过每次都sync会相对好点）
3. 只能测试写

# fio tool in Linux

## fio说明

[参考网上资料](https://wiki.mikejung.biz/Benchmarking#Fio_Test_Options_and_Examples)

我们的设置：
--direct=1，这样，OS的buffer就不起作用
--fsync=0，如果为1，每个block都flush再做下一个。但那样和实际运行不一致，

NOTE: fsync == 0 or == 1的比较
尝试和1做过比较，差别不是太大，稍微有提高。只有sequential write(且bs=4k)时，有5或6倍的巨大差别，怀疑是seqential write时，driver或SSD做了写合并。即bs小时合并才有效，大了就不太容易合并，但奇怪的是seequential read没有什么影响。

然后，我们测试不用的场景，包括：
blocksize，bs=4K, 16K, 128K, 1M
numjobs，多少个file，测试包括1个， 4个, 总大小都是4G
rw，包括randread， randrw(r/w=3:1)， read（sequential), write(sequential)

e.g.

### Random read
```
fio --name=test --rw=randread --bs=4K --direct=1 --numjobs=1 --size=4G --runtime=120 --group_reporting 
fio --name=test --rw=randread --bs=4K --direct=1 --numjobs=4 --size=1G --runtime=120 --group_reporting 
fio --name=test --rw=randread --bs=1M --direct=1 --numjobs=1 --size=4G --runtime=120 --group_reporting 
```
### Rnadom write
fio --name=test --rw=randwrite --bs=4K --direct=1 --numjobs=1 --size=4G --runtime=120 --group_reporting
### Random read/write, read=75% (write=25%, r/w=3:1)
fio --name=test --rw=randrw --rwmixread=75 --bs=4K --direct=1 --numjobs=4 --size=1G --runtime=120 --group_reporting
### Sequential read
fio --name=test --rw=read --bs=4K --direct=1 --numjobs=4 --size=1G --runtime=120 --group_reporting
### Sequential write
fio --name=test --rw=write --bs=4K --direct=1 --numjobs=4 --size=1G --runtime=120 --group_reporting

## 测试结果

| block size | job number	| mode	| read iops	| read bandwith	| write iops | write bandwith |
|:-----------|:-----------|:------|----------:|--------:|-----------:|---------:|
| bs=4K |	numjobs=1 |randread         | 2855 | 11.7MB/s |	
|       |           |randwrite        |      |          | 3109 | 12.7MB/s |	
|	      |	          |randrw(3:1)      |	1949 | 7986kB/s |  652 | 2673kB/s |  
|		    |           |read sequential  |	4429 | 18.1MB/s	|      |          | 
|		    |           |write sequential	|	     |          | 7266 | 29.8MB/s | 
|       |	nunjobs=4	|randread	        | 3941 | 16.1MB/s	|      
|       |           |randwrite        |      |          | 4341 | 17.8MB/s | 
|       |		        |randrw(3:1)	    | 2957 | 12.1MB/s	| 991	 | 4060kB/s |
|		    |           |read sequential	| 3734 | 15.3MB/s	|	
|		    |           |write sequential |	     |          | 12.2k| 49.0MB/s |
|bs=16K	| numjobs=1	|randread         |	2490 | 40.8MB/s	|	
|       |           |randwrite        |      |          | 3754 | 61.5MB/s |
|		    |           |randrw(3:1)      |	1782 | 29.2MB/s	| 595  | 9758kB/s |
|		    |           |read sequential	| 4442 | 72.8MB/s	|	
|       |           |write sequential	|	     |          | 6236 | 102MB/s  |
|       | nunjobs=4	|randread         |	2885 | 47.3MB/s |		
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

## 分析

1. block size越大，带宽也越大
2. 在bs不太大（128KB）前，带宽和Block Size基本是线性的
3. 在不用到buffer情况下，基本很难达到10K IOPS
4. 用buffer情况下，有几十几百K的IOPS（不在报表里），不过，那应该算是幻象，即这么高的IOPS，实际是内存高速的原因导致
5. 