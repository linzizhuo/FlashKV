之前我们讲过了在redis-sds中的分类设计，今天来讲讲new-expand这两个部分。
两者看上去大差不差，但
如果是在分段头设计上sds体现出了一种思想：按需分配，绝不多做事情的原则
那么在new-expand上则体现出了一种：该保守保守，该激进激进的设计哲学，两者本质上是相同的，都是在优先情况下对资源的最优解。
首先声明，这里不会讲sds所有涉及new-expand的实现，而是主要围绕两个核心函数展开——
_sdsnewlen
_sdsMakeRoomFor

补充：因为在拆解的过程中主播不仅接触到了主逻辑，复杂的边界判断，还有很深的设计哲学。
由于主播能力有限，只能从前两方面来讲解。

先来看一下_sdsnewlen的定义：
/* 根据 'init' 指针和 'initlen' 指定的内容创建一个新的 sds 字符串。
 * 如果 'init' 为 NULL，则字符串用零字节初始化。
 * 如果使用 SDS_NOINIT，则缓冲区保持未初始化状态；
 *
 * 字符串始终以 null 结尾（所有 sds 字符串都是如此），因此
 * 即使你这样创建一个 sds 字符串：
 *
 * mystring = sdsnewlen("abc",3);
 *
 * 你也可以用 printf() 打印该字符串，因为字符串末尾隐式包含 \0。
 * 然而该字符串是二进制安全的，中间可以包含 \0 字符，
 * 因为长度存储在 sds 头部中。 */
sds _sdsnewlen(const void *init, size_t initlen, int trymalloc) {
    void *sh;

    char type = sdsReqType(initlen);
    /* 空字符串通常是为了后续追加而创建的。使用 type 8，
     * 因为 type 5 不适合此场景。 */
    if (type == SDS_TYPE_5 && initlen == 0) type = SDS_TYPE_8;
    int hdrlen = sdsHdrSize(type);
    size_t bufsize;

    if (trymalloc) {
        /* 防止 size_t 溢出 */
        if (initlen + hdrlen + 1 <= initlen)
            return NULL;
    } else {
        assert(initlen + hdrlen + 1 > initlen); /* 捕获 size_t 溢出 */
    }
    
    sh = trymalloc?
        s_trymalloc_usable(hdrlen+initlen+1, &bufsize) :
        s_malloc_usable(hdrlen+initlen+1, &bufsize);
    if (sh == NULL) return NULL;

    adjustTypeIfNeeded(&type, &hdrlen, bufsize);
    return sdsnewplacement(sh, bufsize, type, init, initlen);
}
核心有几点：
    1. new后的缓冲区有三种处理方式：1.拷贝，2.0初始化，3.不初始化
    2. trymalloc和非trymalloc，讲size_t溢出和malloc分配视为错误，有两种处理策略：1.内部崩溃，2.优雅降级。
    3. 0初始化的type5主动升级为type8，这涉及type5的设计缺陷和作者的设计哲学，首先空串唯一使用常见就是追加，而type5是没有alloc字段的，他想要获得自己缓冲区的长度需要进行系统调用。这时候设计者认为，与其在等他扩容的时候再进行系统调用，不如在创建阶段就分给他alloc字段，这样用极小的成本规避掉了一次可能性极大的系统调用。
    4. 在一些操作系统中malloc可能会多给一些内存，这部分内存也要充分利用起来。
代码层面的一些技巧：
    initlen + hdrlen + 1 <= initlen，用溢出本身去判断溢出，简洁高效。
    #define s_trymalloc_usable ztrymalloc_usable
    #define s_malloc_usable zmalloc_usable  使用纯宏包装的纯抽象层，使用这种方式不仅可以将模块解耦，还可以很巧妙的将不同操作系统的差异包装在内部。

/* 扩大 sds 字符串末尾的可用空间，使调用者在调用此函数后，
 * 可以安全地在字符串末尾之后覆盖最多 addlen 字节，
 * 外加一个用于 null 终止符的额外字节。
 * 如果已有足够的可用空间，此函数直接返回，不做任何操作；
 * 如果可用空间不足，它会分配所需的空间，甚至可能分配更多：
 * 当 greedy 为 1 时，分配比所需更多的空间，以避免增量增长时未来需要再次重新分配。
 * 当 greedy 为 0 时，仅分配刚好足够的空间来容纳 'addlen'。
 *
 * 注意：这不会改变 sdslen() 返回的 sds 字符串的*长度*，
 * 只会改变我们拥有的可用缓冲区空间。 */
sds _sdsMakeRoomFor(sds s, size_t addlen, int greedy) {
    void *sh, *newsh;
    size_t avail = sdsavail(s);
    size_t len, newlen, reqlen;
    char type, oldtype = sdsType(s);
    int hdrlen;
    size_t bufsize, usable;
    int use_realloc;

    /* 如果剩余空间足够，尽快返回。 */
    if (avail >= addlen) return s;

    len = sdslen(s);
    sh = (char*)s-sdsHdrSize(oldtype);
    reqlen = newlen = (len+addlen);
    assert(newlen > len);   /* 捕获 size_t 溢出 */
    if (greedy == 1) {
        if (newlen < SDS_MAX_PREALLOC)
            newlen *= 2;
        else
            newlen += SDS_MAX_PREALLOC;
    }

    type = sdsReqType(newlen);

    /* 不要使用 type 5：用户正在向字符串追加内容，而 type 5
     * 无法记住空闲空间，因此每次追加操作都必须调用 sdsMakeRoomFor()。 */
    if (type == SDS_TYPE_5) type = SDS_TYPE_8;

    hdrlen = sdsHdrSize(type);
    assert(hdrlen + newlen + 1 > reqlen);  /* 捕获 size_t 溢出 */
    use_realloc = (oldtype == type);
    if (use_realloc) {
        newsh = s_realloc_usable(sh, hdrlen + newlen + 1, &bufsize, NULL);
        if (newsh == NULL) return NULL;
        s = (char*)newsh + hdrlen;
        if (adjustTypeIfNeeded(&type, &hdrlen, bufsize)) {
            memmove((char *)newsh + hdrlen, s, len + 1);
            s = (char *)newsh + hdrlen;
            s[-1] = type;
            sdssetlen(s, len);
        }
    } else {
        /* 由于头部大小发生了变化，需要将字符串向前移动，
         * 并且不能使用 realloc */
        newsh = s_malloc_usable(hdrlen + newlen + 1, &bufsize);
        if (newsh == NULL) return NULL;
        adjustTypeIfNeeded(&type, &hdrlen, bufsize);
        memcpy((char*)newsh+hdrlen, s, len+1);
        s_free(sh);
        s = (char*)newsh+hdrlen;
        s[-1] = type;
        sdssetlen(s, len);
    }
    usable = bufsize - hdrlen - 1;
    assert(type == SDS_TYPE_5 || usable <= sdsTypeMaxSize(type));
    sdssetalloc(s, usable);
    return s;
}
贪婪和非贪婪
    贪婪：在扩容发生时小于SDS_MAX_PREALLOC则给两倍内存，大于SDS_MAX_PREALLOC每次只增加SDS_MAX_PREALLOC，在后续可能需要扩容的场景下使用
    非贪婪：在扩容发生时只申请正好够用的内存，除非明确知道不扩容的情况下不使用。

在扩展时如果type和oldtype是同类型，那么使用realloc就有可能省去一次复制。realloc是c标准库自带的，原地扩展还是搬家由分配器决定，sds不关心。
如果是不同类型，那么就需要malloc，拷贝，free。
和new一样的type5转type8，assert溢出捕获，adjustTypeIfNeeded充分利用缓冲区长度这些。

最后是一些碎碎念：我觉得初次学习，我应该放弃去理解那些面面俱到的完美，因为这段代码在创建之初到现在可能经历了很多次的迭代，里面有很多难以理解的历史遗留因素，甚至某些判断在现在看来是重复的，嗯……在我看来，设计是可以学习的，但代码本身是一次性脑力劳动的结果，我们很难根据一段代码去追溯他本人在写这段代码的时候在想什么。