### lab6 调度器

lab5采用FIFO算法进行进程调度，lab6对其他多种调度算法进行实现

#### 练习1: 使用Round Robin调度算法

##### 调度器sched_class

lab6中，init.c/ken_init()调用了sched.c/sched_init()，初始化调度器sched_class

sched_class结构体的主要部分都是函数指针，即该调度器负责的对就绪队列的操作函数

- init()：初始化运行队列
- enqueue()：将进程p插入队列rq
- dequeue()：将进程p从队列rq中删除
- pick_next()：返回运行队列中下一个可执行的进程
- proc_tick()：timetick处理函数

```c
struct sched_class {
    // 调度器名字
    const char *name;
    void (*init)(struct run_queue *rq);
    void (*enqueue)(struct run_queue *rq, struct proc_struct *proc);
    void (*dequeue)(struct run_queue *rq, struct proc_struct *proc);
    struct proc_struct *(*pick_next)(struct run_queue *rq);
    void (*proc_tick)(struct run_queue *rq, struct proc_struct *proc);
};
```

##### schedule()

trap.c中可以看到，每次中断后，都会检查当前进程是否需要释放CPU（在proc_tick()中设置），如果需要则调用schedule()进行调度

```c
void
trap(struct trapframe *tf) {
    ...
        if (!in_kernel) {
            if (current->flags & PF_EXITING) {
                do_exit(-E_KILLED);
            }
            if (current->need_resched) {
                schedule();
            }
        }
    }
}
```

schedule()会调用各种调度函数sched_class_xxx()进行调度

```c
void
schedule(void) {
    bool intr_flag;
    struct proc_struct *next;
    local_intr_save(intr_flag);
    {
        current->need_resched = 0;
        if (current->state == PROC_RUNNABLE) {//就绪态进程入队
            sched_class_enqueue(current);
        }
        if ((next = sched_class_pick_next()) != NULL) {//选出要运行的进程，出队
            sched_class_dequeue(next);
        }
        if (next == NULL) {//没有进程要执行，就执行idle进程
            next = idleproc;
        }
        next->runs ++;
        if (next != current) {
            proc_run(next);
        }
    }
    local_intr_restore(intr_flag);
}
```

sched_class_xxx()再针对不同的调度器sched_class使用相应的调度算法

sched.c

```c
static inline void
sched_class_enqueue(struct proc_struct *proc) {
    if (proc != idleproc) {
        sched_class->enqueue(rq, proc);
    }
}

static inline void
sched_class_dequeue(struct proc_struct *proc) {
    sched_class->dequeue(rq, proc);
}

static inline struct proc_struct *
sched_class_pick_next(void) {
    return sched_class->pick_next(rq);
}

static void
sched_class_proc_tick(struct proc_struct *proc) {
    if (proc != idleproc) {
        sched_class->proc_tick(rq, proc);
    }
    else {
        proc->need_resched = 1;
    }
}
```

##### proc_struct

为了配合调度器的使用，进程控制块proc_struct也新加入了一些成员变量

- need_resched：需要被释放CPU
- rq：进程运行队列
- run_link：运行队列中的节点
- time_slice：进程剩余的时间片
- lab6_run_pool：该进程在优先队列中的节点
- lab6_stride：该进程的调度步进值
- lab6_priority：该进程的调度优先级

```c
struct proc_struct {
    ...
    volatile bool need_resched;                 // bool value: need to be rescheduled to release CPU?
    struct run_queue *rq;                       // running queue contains Process
    list_entry_t run_link;                      // the entry linked in run queue
    int time_slice;                             // time slice for occupying the CPU
    skew_heap_entry_t lab6_run_pool;            // FOR LAB6 ONLY: the entry in the run pool
    uint32_t lab6_stride;                       // FOR LAB6 ONLY: the current stride of the process 
    uint32_t lab6_priority;                     // FOR LAB6 ONLY: the priority of process, set by lab6_set_priority(uint32_t)
};
```



##### Round Robin调度算法

Round Robin调度算法：

- 所有就绪态进程位于一个就绪队列中，每个运行的进程都被分配一定数量的时间片。
- 每次时钟中断后，时间片数量-1。
- 每当一个进程时间片用完，就将其放到队尾，再取队头进程运行

代码实现：

- stride_enqueue()：新的就绪态进程加入到队尾，并将进程的时间片设为时间片上限
- stride_dequeue()：将队头进程出队
- stride_pick_next()：找到下面执行的进程，RR算法中就是队头进程
- stride_pick_next()：将进程的时间片-1，一旦时间片为0则设定当前进程需要释放CPU。注：该函数只会被run_timer_list()调用

default_sched.c

```c
//入队
static void
stride_enqueue(struct run_queue *rq, struct proc_struct *proc) {
     assert(list_empty(&(proc->run_link)));
     list_add_before(&(rq->run_list), &(proc->run_link));//在rq队头插入节点
     if (proc->time_slice == 0 || proc->time_slice > rq->max_time_slice) {
          proc->time_slice = rq->max_time_slice;//设置进程时间片为时间片上限
     }
     proc->rq = rq;
     rq->proc_num ++;
}

//出队
static void
stride_dequeue(struct run_queue *rq, struct proc_struct *proc) {
     assert(!list_empty(&(proc->run_link)) && proc->rq == rq);
     list_del_init(&(proc->run_link));//队列中删掉队头
     rq->proc_num --;//运行队列中进程数-1
}

//从队头找到下一个执行的进程
static struct proc_struct *
stride_pick_next(struct run_queue *rq) {
     list_entry_t *le = list_next(&(rq->run_list));//头结点的下一节点
     if (le != &rq->run_list){
          return le2proc(le, run_link);
     }
     return NULL;
}

//每次时钟中断后都对进程的时间片-1
static void
stride_proc_tick(struct run_queue *rq, struct proc_struct *proc) {
     if (proc->time_slice > 0) {
          proc->time_slice --;//时间片-1
     }
     if (proc->time_slice == 0) {
          proc->need_resched = 1;//时间片为0，设置当前进程需要释放CPU
     }
}
```

##### 定时器

上述RR算法实现的关键是在每个时钟中断后更新进程时间片，而这需要为每个进程分配一个定时器timer来进行管理，而所有进程的定时器用一个定时器链表timer_list连接

定时器timer_t结构体（sched.h）

- expires：该定时器剩余的时间
- proc：该定时器绑定的进程
- timer_link：定时器链表节点

```c
typedef struct {
    unsigned int expires;       //the expire time
    struct proc_struct *proc;   //the proc wait in this timer. If the expire time is end, then this proc will be scheduled
    list_entry_t timer_link;    //the timer list
} timer_t;
```

在lab6的trap.c/trap_dispatch()中，对于时钟中断的处理函数增加了一行run_timer_list()，该函数位于sched.c

```c
    case IRQ_OFFSET + IRQ_TIMER:
        ticks ++;
        assert(current != NULL);
        run_timer_list();
        break;
```

```c
void
run_timer_list(void) {
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        list_entry_t *le = list_next(&timer_list);
        if (le != &timer_list) {
            timer_t *timer = le2timer(le, timer_link);//找到链表头节点对应的定时器
            assert(timer->expires != 0);
            timer->expires --;//定时器时间片-1
            while (timer->expires == 0) {//一旦当前定时器时间片归零
                le = list_next(le);//遍历链表中定时器，找到时间片大于0的进程
                struct proc_struct *proc = timer->proc;
                if (proc->wait_state != 0) {
                    assert(proc->wait_state & WT_INTERRUPTED);
                }
                else {
                    warn("process %d's wait_state == 0.\n", proc->pid);
                }
                wakeup_proc(proc);//唤醒进程
                del_timer(timer);//删掉时间片归零的定时器
                if (le == &timer_list) {
                    break;
                }
                timer = le2timer(le, timer_link);
            }
        }
        sched_class_proc_tick(current);//调用proc_tick()，将当前进程的时间片-1
    }
    local_intr_restore(intr_flag);
}
```

##### 总结：ucore时钟中断与调度

- 只要发生时钟中断，中断处理函数就调用run_timer_list()，将当前定时器时间片-1，然后调用proc_tick()，将当前进程的时间片-1。（尽管这两个操作完成的事情是相同的，但前者是站在整个CPU的角度分配时间片，后者是进程的角度使用时间片，是两个层面的操作）
- 一旦时间片减到0
  - run_timer_list()遍历定时器链表，删除时间片归零的定时器，并找到下一个时间片大于0的进程，将其唤醒
  - proc_tick()设置当前进程需要释放CPU
- 每个中断处理完成后，CPU都会检查当前进程是否需要释放CPU，是则调用schedule()面对就绪队列进行调度

##### 多级反馈队列调度算法

特点：优先级高、短作业优先

- 设置多个进程运行队列，每个队列有一个优先级，优先级越高的队列上的进程的时间片越短
- 一个队列运行完再运行下一级队列
- 如果一个进程时间片用完也没结束，就进入下一级队列



#### 练习2: 实现 Stride Scheduling 调度算法

（1）每个就绪态进程有一个当前调度权stride

（2）每次调度选择stride最小的进程

（3）进程在调度后，stride+=步长pass

（4）进程运行一段时间后，回到（2）