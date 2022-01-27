#include <list.h>
#include <sync.h>
#include <proc.h>
#include <sched.h>
#include <assert.h>

void
wakeup_proc(struct proc_struct *proc) {
    assert(proc->state != PROC_ZOMBIE && proc->state != PROC_RUNNABLE);
    proc->state = PROC_RUNNABLE;
}

void
schedule(void) {
    bool intr_flag;
    list_entry_t *le, *last;
    struct proc_struct *next = NULL;
    local_intr_save(intr_flag);
    {
        current->need_resched = 0;//当前进程不需调度
        last = (current == idleproc) ? &proc_list : &(current->list_link);//如果当前进程是idle进程，则从链表头开始
        le = last;
        do {
            if ((le = list_next(le)) != &proc_list) {
                next = le2proc(le, list_link);
                if (next->state == PROC_RUNNABLE) {//遍历链表，找到就绪态的进程
                    break;
                }
            }
        } while (le != last);
        if (next == NULL || next->state != PROC_RUNNABLE) {//如果不存在就绪态进程，就运行idle
            next = idleproc;
        }
        next->runs ++;
        if (next != current) {
            proc_run(next);//进程切换
        }
    }
    local_intr_restore(intr_flag);
}

