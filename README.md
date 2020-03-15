## 介绍
* (Memory Pool)是一种内存分配方式。通常我们习惯直接使用new、malloc等API申请分配内存，这样做的缺点在于：由于所申请内存块的大小不定，当频繁使用时会造成大量的内存碎片并进而降低性能。（转自百度百科）

## 基本概念
* 内存池则是在真正使用内存之前，先申请分配一定数量的、大小相等(一般情况下)的内存块留作备用。当有新的内存需求时，就从内存池中分出一部分内存块，若内存块不够再继续申请新的内存。这样做的一个显著优点是，使得内存分配效率得到提升。（转自百度百科）
### 1.数据结构定义
#### **⑴ ngx_pool_t**  内存池主要结构
**① d**  存放数据的
**② large**  当申请内存过大时（大于max），就会走大数据策略
**③ cleanup** 自定义的回调函数，比如可用于文件描述符
```c
struct ngx_pool_s {                   // 内存池主体结构
    ngx_pool_data_t       d;          // 内存池数据结构(存放数据)
    size_t                max;        // 内存池每次可分配的最大内存
    ngx_pool_t           *current;    // 指向当前的内存池首地址
    ngx_chain_t          *chain;      // 缓冲区链表
    ngx_pool_large_t     *large;      // 大数据块分配(当需要分配的内存大于max的时候，走大数据块分配策略)
    ngx_pool_cleanup_t   *cleanup;    // 自定义回调函数，用于清除内存
    ngx_log_t            *log;        // 日志
};                                    // ngx_core.h 里有定义 typedef struct ngx_pool_s ngx_pool_t;
```

#### **⑵ ngx_pool_data_t** 存放数据的
**① last** 指向空闲空间的首地址
**② end** 指向数据结构的末尾（具体看图）
**③ next** 指向下一个池
```c
typedef struct {                      // 内存池数据结构(存放数据)
    u_char               *last;
    u_char               *end;
    ngx_pool_t           *next;
    ngx_uint_t            failed;
} ngx_pool_data_t;
```
#### **⑶ ngx_pool_large_s 大数据块**
**① next** 指向下一个ngx_pool_large_s
**② alloc** 指向数据块
```c
struct ngx_pool_large_s {
    ngx_pool_large_t     *next;
    void                 *alloc;
};
```

#### **⑷ ngx_pool_cleanup_s 可自定义回收内存**
**① handler** 需要注册的回调函数
**② data** 需要回收的数据
**③ next** 指向下一个ngx_pool_cleanup_s
```c
struct ngx_pool_cleanup_s {
    ngx_pool_cleanup_pt   handler;
    void                 *data;
    ngx_pool_cleanup_t   *next;
};
```
**内存池结构：**
![ngx内存池.jpg](https://upload-images.jianshu.io/upload_images/18154407-f7547b621a07b9ad.jpg?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)            [看不到图片点这](https://upload-images.jianshu.io/upload_images/18154407-f7547b621a07b9ad.jpg?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)（我要研究下怎么把图片放到github）

### 2.函数实现
#### **⑴ 创建内存池**
**这里注意的是：**
**① ngx_memalign() 是根据不同的操作系统做不同的申请内存的操作**
```c
ngx_pool_t *
ngx_create_pool(size_t size, ngx_log_t *log)  // 创建一个内存池
{
    ngx_pool_t  *p;

    // 这里的ngx_memalign()有区别
    // 如果有NGX_HAVE_POSIX_MEMALIGN，则走posix_memalign
    // 如果有NGX_HAVE_MEMALIGN，则走memalign
    // 都没有，则走ngx_alloc
    // 这里的NGX_HAVE_POSIX_MEMALIGN和NGX_HAVE_MEMALIGN是根据不同操作系统来区分的
    // 操作系统：Linux、Solaris、FreeBSD
    p = ngx_memalign(NGX_POOL_ALIGNMENT, size, log);      // NGX_POOL_ALIGNMENT = 16
    if (p == NULL) {
        return NULL;
    }

    p->d.last = (u_char *) p + sizeof(ngx_pool_t); // 结构体之后的数据起始位
    p->d.end = (u_char *) p + size;                // 结束的地址
    p->d.next = NULL;                              // 下一个ngx_pool_t
    p->d.failed = 0;                               // 分配内存失败次数

    size = size - sizeof(ngx_pool_t);              // 分配完结构体里基本数据后，还剩下多少内存可以给其他服务使用
    p->max = (size < NGX_MAX_ALLOC_FROM_POOL) ? size : NGX_MAX_ALLOC_FROM_POOL; // NGX_MAX_ALLOC_FROM_POOL = 4095(x86架构下)

    p->current = p;    // 指向当前ngx_pool_data_t的起始地址
    p->chain = NULL;   // 缓冲区
    p->large = NULL;   // 大数据块分配策略
    p->cleanup = NULL; // 自定义回调函数，回收内存
    p->log = log;      // 日志

    return p;
}
```

#### **⑵ 销毁内存池**
**这里注意的是回收内存的顺序，和c->handler回调函数**
**下面这语句有点意思。**
```c
typedef void (*ngx_pool_cleanup_pt)(void *data); // 回调
```
举个例子：
```c
原函数是 void func(void);
typedef void (*ngx_pool_cleanup_pt)(void);
然后ngx_pool_cleanup_pt handler = func;
调用handler(void);
```
**ngx_memalign() 是根据不同的操作系统做不同的申请内存的操作**
```c
void
ngx_destroy_pool(ngx_pool_t *pool)  //销毁内存池
{
    ngx_pool_t          *p, *n;
    ngx_pool_large_t    *l;
    ngx_pool_cleanup_t  *c;

    // 先清理cleanup
    for (c = pool->cleanup; c; c = c->next) {
        // handler为清理的回调函数
        if (c->handler) {
            ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                           "run cleanup: %p", c);
            c->handler(c->data);
        }
    }

#if (NGX_DEBUG)

    /*
     * we could allocate the pool->log from this pool
     * so we cannot use this log while free()ing the pool
     */

    for (l = pool->large; l; l = l->next) {
        ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0, "free: %p", l->alloc);
    }

    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
        ngx_log_debug2(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                       "free: %p, unused: %uz", p, p->d.end - p->d.last);

        if (n == NULL) {
            break;
        }
    }

#endif
    // 清理大数据块
    for (l = pool->large; l; l = l->next) {
        if (l->alloc) {
            ngx_free(l->alloc);
        }
    }
    // 清理非大数据块分配的内存——走正常分配策略的内存
    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
        ngx_free(p);

        if (n == NULL) {
            break;
        }
    }
}
```

#### **⑶ 重置内存池**
**这里需要注意的是：大数据块的内存需要回收，而内存池里只改变last指针**
```c
void
ngx_reset_pool(ngx_pool_t *pool)    // 重设内存池，和销毁差不多
{
    ngx_pool_t        *p;
    ngx_pool_large_t  *l;

    // 大数据需要释放
    for (l = pool->large; l; l = l->next) {
        if (l->alloc) {
            ngx_free(l->alloc);
        }
    }

    // 正常分配策略的数据不需要释放，只需要改变指针位置即可
    for (p = pool; p; p = p->d.next) {
        p->d.last = (u_char *) p + sizeof(ngx_pool_t);
        p->d.failed = 0;
    }

    pool->current = pool;
    pool->chain = NULL;
    pool->large = NULL;
}
```

#### **⑷ 分配内存**
**这里需要注意的是：ngx_align_ptr做了对齐操作**
last指针需要移动到可使用内存块的首地址
```c
static ngx_inline void *
ngx_palloc_small(ngx_pool_t *pool, size_t size, ngx_uint_t align)
{
    u_char      *m;
    ngx_pool_t  *p;

    p = pool->current;

    do {
        m = p->d.last;

        if (align) {
            // 对齐，加快访问速度
            m = ngx_align_ptr(m, NGX_ALIGNMENT);
            // ngx_align_ptr 和 ngx_align 差不多，不介绍ngx_align_ptr了
            // ngx_align(d, a)     (((d) + (a - 1)) & ~(a - 1))
            // 假设a=8 a为2的幂
            // & 后面的值为-8  (111111000)
            // 假设d=74
            // 74 + 8 = 01010010
            // 01010010 & 111111000 = 01010000
            // 计算出来的结果是80，对齐之后可以加快访问速度
        }

        if ((size_t) (p->d.end - m) >= size) {
            p->d.last = m + size;

            return m;
        }

        p = p->d.next;

    } while (p); // 找到符合大小的内存块

    return ngx_palloc_block(pool, size);  // 没有满足要求，则需要进行扩容
}
```

#### **⑸ 扩容**
**ngx_pool_t 里的 d.next 指向当前扩容的，具体看上面的图（红色的箭头）**
```c
static void *
ngx_palloc_block(ngx_pool_t *pool, size_t size)  // 扩容
{
    u_char      *m;
    size_t       psize;
    ngx_pool_t  *p, *new;

    // 计算当前内存池大小
    psize = (size_t) (pool->d.end - (u_char *) pool);
    // 申请新的内存块，大小与pool相同
    m = ngx_memalign(NGX_POOL_ALIGNMENT, psize, pool->log);
    if (m == NULL) {
        return NULL;
    }

    new = (ngx_pool_t *) m;

    new->d.end = m + psize; // 设置end指针
    new->d.next = NULL;
    new->d.failed = 0;

    m += sizeof(ngx_pool_data_t); // m指向ngx_pool_data_t之后的起始位置
    m = ngx_align_ptr(m, NGX_ALIGNMENT); // 对齐
    new->d.last = m + size; // last指针指向还没分配的起始位 m 为已经分配的起始位

    for (p = pool->current; p->d.next; p = p->d.next) {
        if (p->d.failed++ > 4) {  // failed只在这里做修改
            pool->current = p->d.next;  // 失败4次以上移动current
        }
    }

    p->d.next = new;

    return m;
}
```

#### **⑹ 分配大数据块**
**大数据块是放到ngx_pool_t 里的 large 管理的**
**这里有点意思，有尾插法和头插法，运用在不同情况**
**当large链上节点少于等于4且alloc为NULL时，用尾插法，否则使用头插法**
```c
static void *
ngx_palloc_large(ngx_pool_t *pool, size_t size)  // 申请大数据块
{
    void              *p;
    ngx_uint_t         n;
    ngx_pool_large_t  *large;

    // 分配一大块内存
    p = ngx_alloc(size, pool->log);
    if (p == NULL) {
        return NULL;
    }

    n = 0;

    // 往下20行的代码有点意思
    // 在回收large的时候，只是清除alloc，而next是还在的
    // 所以这里判断有next但没有alloc(3次内)，符合就将p置于alloc
    // 这里是尾插法
    for (large = pool->large; large; large = large->next) {
        if (large->alloc == NULL) {
            large->alloc = p;
            return p;
        }

        if (n++ > 3) {  // 为什么是3？估计是经验所得
            break;
        }
    }

    // 分配内存
    large = ngx_palloc_small(pool, sizeof(ngx_pool_large_t), 1);
    if (large == NULL) {
        ngx_free(p);
        return NULL;
    }

    // 头插法
    large->alloc = p;
    large->next = pool->large;
    pool->large = large;

    return p;
}
```
---
2020.1.16  19:39  广州

