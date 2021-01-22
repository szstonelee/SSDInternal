
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

# 测试结果

| 测试模式 | 第一次测试(ms) | 第二次测试(ms) | 第三次测试(ms) |
| -- | -- | -- | -- |
| new file without fallocate | 164259 | 164290 | 158285 |
| new file with fallocate, NoneZero | 201633 | 159954 | 162243 |
| new file with fallocate, FillZero | 183079 | 165731 | 185302 |
| overwrite with fallocate | 166648 | 157356 | 182170 |
| overwrite without fallocate | 91631 | 80919 | 0 |