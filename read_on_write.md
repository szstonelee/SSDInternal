# read on write和更多的深挖

# 参考知乎一文章

知乎里一篇关于磁盘写的文章，["read-on-write" in InnoDB](https://zhuanlan.zhihu.com/p/61002228)。

文章内容很深，机理描述如下：

如果向一个磁盘每次写512字节（可以理解sector单位，如果是HHD的话，硬件内部最小写的粒子单位就是sector大小，但文中所用磁盘是SSD，SSD没有sector概念，只有page硬件粒子大小的读写，但可以用512字节作为一个最小计数单位去衡量所有磁盘类型的读写），连续写。由于对于操作系统而言，磁盘最小的读写单位就是page，一般是4096字节，因此对于每个page的第一个写，在内部磁盘IO时，会导致出现一个读。为了减少这个读，优化的程序在每8个写的第一个写时，用了4096个字节而不是512字节，因此减少了这个读，从而提高了40%的性能。

t.cc是没有用优化的code，因此可以在iostat里发现有读，同时用blktrace也可以看到这样的读证明
```
 259,6    0      184     0.001777196 58995  A   R 55314456 + 8 <- (259,9) 55312408
 259,9    0      185     0.001777463 58995  Q   R 55314456 + 8 [a.out]
 259,9    0      186     0.001777594 58995  G   R 55314456 + 8 [a.out]
```

而t2.cc是优化的版本，不管是iostat，还是blktrace，都看不到这样的读迹象。

blktrace的命令如下: 
```
blktrace -d /dev/vda -o - | blkparse -i -
```
NOTE: 你需要用你自己的device，我的是vda，你自己的，请用```ls /dev```进行检查

# 我的实验

我对照写了几个程序，去印证这个事，test_row.cc是完全copy原来的程序t.cc，为了证明我的机器也是如此的表现。而且确实也如此。

然后，我写了[read_on_write.cpp](read_on_write.cpp)，去进一步优化代码和深挖一些东西，发现了一些有趣的东西。

## 原来的文件必须存在

我开始犯的第一个错误是，没有产生overwrite的文件。因为如果没有全新的一个文件，就不是overwrite，那么Linux认为即使是对于page写512字节，不用去读，自动补齐后面的字节即可（理论上补齐应该是全0）。

## page cache必须清除

我们分析一下t.cc：

对于一个page（4096字节）的8个sector（512字节）单位的写，如果每次写都是sector单位，那么只有第一次需要读，这是因为有page cache。

对于每个page，第一次sector大小的overwrite写，因为内部实际要写的单位是page大小，所以OS必须从磁盘读出这个page，然后混合（改写overwrite前512字节），最后按page大小写入，同时这个page就会留在内存里，i.e. page cache里，后面的7个sector写，都可以和这个内存里的page混合，再按4096字节去写，因此可以省略后面的7个磁盘读。

所以，测试时，必须有旧文件，同时必须清除page cache，否则你观测不到想要的结果。

同时，OS对于读有很多优化，比如预读read ahead，就是读的指令不会只读当前的page，可以连续读一批page，因为消耗差不多，这对于HDD非常有效（对SSD其实也是有效的）。这个参数在OS里的配置叫read_ahead_kb。

为了观测准确，我们需要关闭这个优化。

请参考read_on_write.cpp里面的clear_page_cache()

## 首次创建的文件必须真实写过东西

发现必须有旧文件时，我最开始是写了下面这些code去做修正
```
// 如果测试文件找不到的话，需要第一次创建文件
ret = fseek(fp, file_size - 1, SEEK_SET);   // file_size是要测试的文件大小，原文是1G，我的测试程序里是500M
assert(ret == 0);
ret = fwrite("", 1, sizeof(char), fp);
assert(ret == 1);
ret = fclose(fp);
assert(ret == 0);
```

这段代码可以很快形成一个我要大小的旧文件，即它先跳到，i.e., fseek()，文件最后位置之前一个字节，然后写入一个字节内容。

实际发现这样根本不行，即使清除过cache，然后重新open file，仍然产生不了我们想观测的read证据。

为什么？

我认为Linux是个足够聪明的文件系统，当用上面的代码产生文件时，虽然文件的meta data写明这是个500M大小的文件，也分配（或预留）了磁盘空间给文件，比如几个extent和block，但这几个extent和block是否被写过(或者被初始化过，因为ext4可以delay文件相应的extent和block的初始化)，Linux是知道的。所以，当程序发出overwrite指令时，Linux内部是可以知道这是overwrite，还是new write。

所以，我最后的修订是：在check_file_before_write()里实际写入（有fwrite）。由于block size是1M，而且不是每次sync，所以写入是很快的。

## 测试文件的大小

刚开始时，我测试时用了100M，发现测试结果是差不多，并没有显示节省了read的方法要快一些。后来改成500M，才有了效果。

为什么会这样？

在前面我们说过，OS有read ahead的优化，但其实，在SSD内部，一样有read ahead，即你读某个page，在SSD内部，也是连续读出几个page（或几十个, who knows），因为消耗是差不多的，并且缓存在SSD内部的SDRAM里，这样如果下次读cache hit，就非常快，因为从SSD内部的RAM走，而不是缓慢的NAND物理介质。 所以，真正耗时的每次基于NAND的实际read，要少很多。

这样，如果测试文件过小，效果就不明显。只有放大测试文件，让实际read有一定量，才能显示优化的效果。

## O_DIRECT

我还尝试了O_DIRECT模式，即不用page cache。

注意：Linux里说direct模式是尽量少用，不代表不用。

实际结果也很有意思，没有read了，即使clear page cache。

到底为什么会这样，我还没有想明白。一种可能是：direct有自己的cache(可以利用page os cache)，不受clear page cache的影响。

从下面的执行效果看，direct是最不稳定，有时比buffered no read好，有时却和buffered with read一样坏。

我甚至写了[test_iow_by_direct.cpp](test_iow_by_direct.cpp)去尝试发觉：

1. 是否会出现read在blktrace里

2. 或者是否会出现写错整个page的现象（因为我用512字节'b'去写4096字节原来是'a'的文件，如果没有read以及相应的overwrite，那么文件的尾部就不会全是字符’a‘）

仍然无结果：既没有read在blktrace里，也没有写错文件。这也是我上面可能的猜测的原因。

## 测试结果

NOTE: 测试执行需要```sudo ./a.out```，因为要清除page cache，需要sudo权限

编译执行
```
g++ -std=c++17 read_on_write.cpp
sudo ./a.out 1 (or 2 or 3)
```

| 测试模式 | 测试编号 | 结果(ms) |
| -- | -- | -- |
| buffered with read | 1 | 926949 |
|                    | 2 | 870821 |
|                    | 3 | 847956 |
| buffered without reead | 1 | 679994 |
|                        | 2 | 691255 |
|                        | 3 | 767706 |
| no buffered (direct) | 1 | 590019 |
|                      | 2 | 706398 |
|                      | 3 | 875757 |