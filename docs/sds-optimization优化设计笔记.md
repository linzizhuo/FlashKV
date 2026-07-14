目前已经将分段头和静态解析加入到了源码中。
接下来是设计方案：

首先分析更改范围：

sdsnew 涉及长度确定，长度变更
sdsnewlen 同上
void* sds sdsdup(const sds s) 职责：拷贝，复制，不涉及长度变更
uint64_t sdsHash(const void *key) 职责：hash，不涉及长度变更
sdsCompare 职责：比较，不涉及长度变更
sdsfree 不涉及长度变更
size_t sdsSerialize(const sds s, void **buf); 职责：序列化，不涉及长度变更
sds sdsDeserialize(const void *buf); 职责：反序列话，可能涉及长度确定和长度变更，但应该被new抽象掉
int sdsWrite(Io *io, sds s); /* [4B len][data] 直接进 io */
int sdsRead(Io *io, sds *s); // 读：返回字节数或 ERR，sds 由 *s 带出  这部分同上

精细判断是否需要更改：
sdsnewlen 需要
sdsfree 需要
实际需要更改的函数是很少的
当然在设计上也发现了一个缺陷：rdb中数据长度是硬编码，和现在sds不是很适配，可以根据数据结构自治序列化的思想进行优化。

反思：为何粗略分析和实际分析存在差异？

1. 历史问题，编写时的设计思路和分析时可能不一样。
2. 基本功：在部分场景没有成熟的方法，可能能够很简单的分析出流程，但没有规范的抽象的方法，这也是我认为目前的缺陷，有算法基础，无工程直觉。
3. 命名规范？可能有这部分的问题。

如何提升准确率？

从问题入手寻找答案，规范命名，注释，系统锻炼工程直觉。

反思一下提升准确率是否有必要
就目前水平来说，我不确定，但我感性觉得有权衡的设计在目前来看应该是好的（）

优化方法：
通过阅读redis的源码，我发现了一种比较简单的方法：
外部接口和内部实现分离，比如 sdsnewlen 和 _sdsnewlen
乍一看这两个是一样的，sdsnewlen也只是调用了 _sdsnewlen，但这其实是一种抽象分离，我不确定这样做是否是好的，但可以试着尝试。

改造 sdsnewlen：类型选择 + _sdsnewlen 分离
修复 sdsfree：用 sdsHdrSize 替换 SDS_HDR(64)
改造 sdsdup / sdsDeserialize / sdsRead：过一遍确认是否受影响（可能会影响rdb，但目前的抽象选择貌似不会影响外层，只是可能要改一下文档，这就是抽象的魅力啊~）