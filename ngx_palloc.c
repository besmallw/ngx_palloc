
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


static ngx_inline void *ngx_palloc_small(ngx_pool_t *pool, size_t size,
    ngx_uint_t align);
static void *ngx_palloc_block(ngx_pool_t *pool, size_t size);
static void *ngx_palloc_large(ngx_pool_t *pool, size_t size);


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


void *
ngx_palloc(ngx_pool_t *pool, size_t size)
{
#if !(NGX_DEBUG_PALLOC)
    if (size <= pool->max) {   // 由内存池正常分配内存
        return ngx_palloc_small(pool, size, 1);  // 1 需要内存对齐
    }
#endif

    // 需求的内存过大，走大数据块分配策略
    return ngx_palloc_large(pool, size);
}


void *
ngx_pnalloc(ngx_pool_t *pool, size_t size)
{
#if !(NGX_DEBUG_PALLOC)
    if (size <= pool->max) {   // 由内存池正常分配内存
        return ngx_palloc_small(pool, size, 0);  // 0 不需要内存对齐
    }
#endif

    return ngx_palloc_large(pool, size);
}


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


void *
ngx_pmemalign(ngx_pool_t *pool, size_t size, size_t alignment)
{
    void              *p;
    ngx_pool_large_t  *large;

    p = ngx_memalign(alignment, size, pool->log);
    if (p == NULL) {
        return NULL;
    }

    large = ngx_palloc_small(pool, sizeof(ngx_pool_large_t), 1);
    if (large == NULL) {
        ngx_free(p);
        return NULL;
    }

    large->alloc = p;
    large->next = pool->large;
    pool->large = large;

    return p;
}


ngx_int_t
ngx_pfree(ngx_pool_t *pool, void *p)  // 释放指定的一块大数据块
{
    ngx_pool_large_t  *l;

    for (l = pool->large; l; l = l->next) {
        if (p == l->alloc) {
            ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                           "free: %p", l->alloc);
            ngx_free(l->alloc);
            l->alloc = NULL;

            return NGX_OK;
        }
    }

    return NGX_DECLINED;
}


void *
ngx_pcalloc(ngx_pool_t *pool, size_t size)
{
    void *p;

    p = ngx_palloc(pool, size);
    if (p) {
        ngx_memzero(p, size);
    }

    return p;
}


ngx_pool_cleanup_t *
ngx_pool_cleanup_add(ngx_pool_t *p, size_t size)  // 添加一个用于清理自定义操作的cleanup
{
    ngx_pool_cleanup_t  *c;

    // 用于回调函数清理内存块
    c = ngx_palloc(p, sizeof(ngx_pool_cleanup_t));
    if (c == NULL) {
        return NULL;
    }

    if (size) {
        c->data = ngx_palloc(p, size);
        if (c->data == NULL) {
            return NULL;
        }

    } else {
        c->data = NULL;
    }

    // 回调函数
    c->handler = NULL;

    // 头插法
    c->next = p->cleanup;
    p->cleanup = c;

    ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, p->log, 0, "add cleanup: %p", c);

    return c;
}


void
ngx_pool_run_cleanup_file(ngx_pool_t *p, ngx_fd_t fd)   // 手动清理，主要是fd  (销毁内存池的时候也会自动清理cleanup)
{
    ngx_pool_cleanup_t       *c;
    ngx_pool_cleanup_file_t  *cf;

    for (c = p->cleanup; c; c = c->next) {
        if (c->handler == ngx_pool_cleanup_file) {  // 调用ngx_pool_cleanup_file()回调函数清理

            cf = c->data;

            if (cf->fd == fd) {
                c->handler(cf);
                c->handler = NULL;
                return;
            }
        }
    }
}


void
ngx_pool_cleanup_file(void *data)  // 文件句柄通用回调函数
{
    ngx_pool_cleanup_file_t  *c = data;

    ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, c->log, 0, "file cleanup: fd:%d",
                   c->fd);

    if (ngx_close_file(c->fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", c->name);
    }
}

/*
    在ngx_file.c里有
    判断如果是clean的话，就回调到ngx_pool_cleanup_file()
    否侧回调到ngx_pool_delete_file()
*/
void
ngx_pool_delete_file(void *data)
{
    ngx_pool_cleanup_file_t  *c = data;

    ngx_err_t  err;

    ngx_log_debug2(NGX_LOG_DEBUG_ALLOC, c->log, 0, "file cleanup: fd:%d %s",
                   c->fd, c->name);

    if (ngx_delete_file(c->name) == NGX_FILE_ERROR) {
        err = ngx_errno;

        if (err != NGX_ENOENT) {
            ngx_log_error(NGX_LOG_CRIT, c->log, err,
                          ngx_delete_file_n " \"%s\" failed", c->name);
        }
    }

    if (ngx_close_file(c->fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", c->name);
    }
}


#if 0

static void *
ngx_get_cached_block(size_t size)
{
    void                     *p;
    ngx_cached_block_slot_t  *slot;

    if (ngx_cycle->cache == NULL) {
        return NULL;
    }

    slot = &ngx_cycle->cache[(size + ngx_pagesize - 1) / ngx_pagesize];

    slot->tries++;

    if (slot->number) {
        p = slot->block;
        slot->block = slot->block->next;
        slot->number--;
        return p;
    }

    return NULL;
}

#endif
