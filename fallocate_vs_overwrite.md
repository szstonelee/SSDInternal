
# 知乎上一篇文章：write file faster

请先参考知乎上的这篇文章[write file faster](https://zhuanlan.zhihu.com/p/61212603)。非常好和深的内容。

其核心点是：当我们写文件时，文件系统filesystem是需要分配空间给我们的写的，有多种方式，那种最快，差别有多大。

比如：我们写1G的内容，顺次写，每次写都是4K大小，而且必须每次写完都sync。大家可以想象一下，对于数据库里的WAL文件, Write Ahead Log，就很可能是这样的工作场景。

按文中总结，总共有五种可能方式：

1. 创建全新文件后，不会调用fallocate()，每次4K调用write()和sync()

2. 创建全新文件，用fallocate()预分配空间（这里是1G），然后每4K的write()+sync()

3. 创建全新文件，用fallocate()带参数FALLOC_FL_ZERO_RANGE，i.e., fallocate + filling zero，后面一样

4. 用一个旧文件，i.e.，已存在的1G文件（我们可以假设是上一次写完留下的），同时不调用fallocate()，然后4K的write()+sync()

5. 用一个旧文件，但同时也调用fallocate(FALLOC_FL_ZERO_RANGE)，然后4K的write()+sync()

前面三种，作者称之为append模式，后面两种则是overwrite模式。然后作者比较了其中三种结果（具体是对应哪三种，我不得而知），所以我写了程序去验证一下。

请参考代码：[fallocate_append_write_and_overwrite.cpp](fallocate_append_write_and_overwrite.cpp)

# 测试结果

## 有文件（但新文件也会倍代码用unlink删除）的测试

| 测试模式 | 第一次测试(ms) | 第二次测试(ms) | 第三次测试(ms) |
| -- | -- | -- | -- |
| new file without fallocate | 164259 | 164290 | 158285 |
| new file with fallocate, NoneZero | 201633 | 159954 | 162243 |
| new file with fallocate, FillZero | 183079 | 165731 | 185302 |
| overwrite with fallocate | 166648 | 157356 | 182170 |
| overwrite without fallocate | 91631 | 80919 | 94356 |

## 每次测试前都会手工rm所有文件的测试

| 测试模式 | 第一次测试(ms) | 第二次测试(ms) | 第三次测试(ms) |
| -- | -- | -- | -- |
| new file without fallocate | 173577 | 170137 | 179816 |
| new file with fallocate, NoneZero | 193260 | 187639 | 169492 |
| new file with fallocate, FillZero | 198008 | 165617 | 206342 |
| overwrite with fallocate | 147408 | 195468 | 191297 |
| overwrite without fallocate | 75394 | 117594 | 82227 |

## 改变次序

由于怀疑SSD有GC的影响，怀疑第一个任务类型，即上面的new file without fallocate，可能会更快些，所以，调整了次序，再测。

| 测试模式 | 第一次测试(ms) | 第二次测试(ms) | 第三次测试(ms) |
| -- | -- | -- | -- |
| new file with fallocate, FillZero  | 158353 | 164308 | 175835 |
| new file with fallocate, NoneZero | 169505 | 174326 | 151264 |
| new file without fallocate | 199230 | 167699 | 179450 |
| overwrite without fallocate | 86065 | 72755 | 83881 |
| overwrite with fallocate | 175609 | 183144 | 180175 |

## 结论

1. 最明显的是overwrite without fallocate，是其他模式的一半或接近一半。所以，没有分配和fill zero这些meta data的消耗，是提升性能最大的关键。

2. 其他四种模式之间基本在一个量级，而且各个测试显示数据有飘忽，有时这个好，有时那个妙。i.e., 有meta data的负担都会影响性能，哪个影响更大，不定，或不稳。

3. SSD的GC可能会影响结果，从上面看，如果是第一个执行动作，数据稍微好些，但不代表所有的数据都好，只是总体感觉稍好。也许是GC的原因，也许是其他，不知。
