#ifndef _LINUX_KCOMPRESS_H
#define _LINUX_KCOMPRESS_H

#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/sched.h>

#define KCOMPRESS_FIFO_SIZE 256

struct kfifo;

struct kcompress_data {
    wait_queue_head_t kcompressd_wait;
    struct task_struct *kcompressd;
    struct kfifo *kcompress_fifo;
    spinlock_t kcompress_fifo_lock;
};

#endif /* _LINUX_KCOMPRESS_H */
