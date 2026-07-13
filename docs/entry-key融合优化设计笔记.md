struct __attribute__((__packed__)) sdshdr64
{
    uint64_t len; // 长度
    uint64_t alloc; // 空间大小
    unsigned char flags; // 标记种类
    char buf[]; // 柔性数组
};
这是现有的sds结构体，是通用的可变数组，但目前应用在我们的Entry上有一个问题。
struct dictEntry
{
    hash_t hash; // 缓存 hash，rehash 直接 & sizemask
    void *key;
    void *val; // 对整数进行了优化，因为void* 和 long long长度相等，所以直接将 val作为整数值使用，即void* -> long long。
    struct dictEntry *next;
};
dictentry定义是hash表上的一个节点，但其实我们都知道，hash表的key是不可变的。
所以对于hash表的key来说 alloc 完全是冗余的，这是一个值得优化的点。所以对于key来说，实际的需求是这样的

struct __attribute__((__packed__)) sdshdr64
{
    unsigned char flags; // 标记种类
    uint64_t len; // 空间大小
    char buf[]; // 柔性数组
};

另一个方向是我们先在new的链路，一个节点要new四次。
entry一次，key一次，val_obj一次，val一次。十分浪费。为了减少malloc次数，我们对这部分逻辑进行优化。

最初的设计是将entry,key,valobj,val全部融于一炉，使用柔性数组+路由层+函数回调的方式实现一次malloc。
但考虑到key,val的长度在使用的时候都是不可控制的，强行融合可能会导致大内存块多，小内存片段难以充分利用的情况，所以舍弃。
退而求其次，我想到了另一种设计。

entry-key绑定，valobj-val绑定。
一方面，entry和valobj的长度是固定的，而且也不大——这一点valobj尤其明显，只有一个types，所以融合在一起是十分必要的。
valobj-val的融合是后话，我们先来讨论如何将entry和key融合。
我的方案是使用柔性数组，将原本的
struct dictEntry
{
    hash_t hash; // 缓存 hash，rehash 直接 & sizemask
    void *key;
    void *val; // 对整数进行了优化，因为void* 和 long long长度相等，所以直接将 val作为整数值使用，即void* -> long long。
    struct dictEntry *next;
};
修改为
struct dictEntry
{
    hash_t hash; // 缓存 hash，rehash 直接 & sizemask
    struct dictEntry *next;
    void *val; // 对整数进行了优化，因为void* 和 long long长度相等，所以直接将 val作为整数值使用，即void* -> long long。
    char key[];
};
这也key和entry的生命周期是绑定的，可以减少一次malloc，坏处也是明显的，key不能随便取用了，大数据量可能会产生额外的拷贝成本，不过仔细想想其实可以解决，
用#define key p+24，这点不过多展开。

但考虑到这个优化对架构影响较大且实际优化并不明显，因此暂不执行。