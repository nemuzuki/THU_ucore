### lab7 同步互斥

#### 练习1: 基于内核级信号量的哲学家就餐问题

ucore对于哲学家就餐问题采用的是【将拿起左右叉子作为整体】的解决方法。

##### 信号量semaphore

每种临界资源都有一个记录型信号量semaphore_t（sem.h）

- value：资源数
- wait_queue：该资源的阻塞队列

```c
typedef struct {
    int value;
    wait_queue_t wait_queue;
} semaphore_t;
```

对临界资源的请求和释放称为PV操作，伪代码：

```c
//请求资源
P(S){
    S.value--;
    if(S.value<0){
        当前进程加入阻塞队列S.L
        block(S.L);//阻塞进程
    }
}
//释放资源
V(S){
    S.value++;
    if(S.value<=0){
        从阻塞队列中移出一个进程P
        wakeup(P);//唤醒进程P，阻塞态到就绪态
    }
}
```

lab7的init进程调用check_sync()来处理哲学家就餐问题

```c
static int
init_main(void *arg) {
    ...
 extern void check_sync(void);
    check_sync();                // check philosopher sync problem

    while (do_wait(0, NULL) == 0) {
        schedule();
    }
    return 0;
}
```

check_sync()函数的前半部分使用信号量来解决哲学家就餐问题

这里设置了n+1个信号量

- mutex用来对多行代码进行原子化，初始为1
- s[i]则表示第i个哲学家是否可以就餐，初始为0（该信号量在phi_test_sema(i)中使用）

```c
void check_sync(void){

    int i;

    //check semaphore
    sem_init(&mutex, 1);//初始化互斥信号量
    for(i=0;i<N;i++){
        sem_init(&s[i], 0);//初始化哲学家行为信号量
        int pid = kernel_thread(philosopher_using_semaphore, (void *)i, 0);//哲学家就餐问题内核线程
        if (pid <= 0) {
            panic("create No.%d philosopher_using_semaphore failed.\n");
        }
        philosopher_proc_sema[i] = find_proc(pid);
        set_proc_name(philosopher_proc_sema[i], "philosopher_sema_proc");
    }
}
```

##### philosopher_using_semaphore()：哲学家行为函数

- 每个哲学家用sleep模拟思考一段时间
- 尝试用phi_take_forks_sema()函数拿起左右两个叉子
- 如果成功，就用sleep模拟就餐一段时间
- phi_put_forks_sema()放下左右两个叉子

```c
int philosopher_using_semaphore(void * arg) /* i：哲学家号码，从0到N-1 */
{
    int i, iter=0;
    i=(int)arg;
    cprintf("I am No.%d philosopher_sema\n",i);
    while(iter++<TIMES)
    { /* 无限循环 */
        cprintf("Iter %d, No.%d philosopher_sema is thinking\n",iter,i); /* 哲学家正在思考 */
        do_sleep(SLEEP_TIME);//sleep模拟思考
        phi_take_forks_sema(i); /* 需要两只叉子，或者阻塞 */
        cprintf("Iter %d, No.%d philosopher_sema is eating\n",iter,i); /* 进餐 */
        do_sleep(SLEEP_TIME);//sleep模拟就餐
        phi_put_forks_sema(i); /* 把两把叉子同时放回桌子 */
        
    }
    cprintf("No.%d philosopher_sema quit\n",i);
    return 0;    
}

```

phi_take_forks_sema()函数中可以看到先用down和up两个函数夹起了两行代码，这正是PV操作的互斥使用方法，使中间的代码不可分割。

```c
//想要拿起两个叉子
void phi_take_forks_sema(int i) /* i：哲学家号码从0到N-1 */
{
        down(&mutex); /* 进入临界区,P(mutex) */
        state_sema[i]=HUNGRY; /* 记录下哲学家i饥饿的事实 */
        phi_test_sema(i); /* 试图得到两只叉子 */
        up(&mutex); /* 离开临界区,V(mutex) */
        down(&s[i]); /* 如果得不到叉子就阻塞,P(s[i]) */
}
```

##### down()：临界资源的P操作

如果信号量大于等于1，则直接-1；否则该进程被阻塞，让CPU调度其他进程，调度完成后会回到该进程，将该进程移出阻塞队列

```c
//临界资源的P操作
static __noinline uint32_t __down(semaphore_t *sem, uint32_t wait_state) {
    bool intr_flag;
    local_intr_save(intr_flag);//关中断
    if (sem->value > 0) {//如果信号量大于等于1，则直接-1
        sem->value --;
        local_intr_restore(intr_flag);
        return 0;
    }
    wait_t __wait, *wait = &__wait;
    wait_current_set(&(sem->wait_queue), wait, wait_state);//如果信号量小于等于0，将当前进程加入阻塞队列
    local_intr_restore(intr_flag);

    schedule();//CPU调度执行其他进程

    local_intr_save(intr_flag);//CPU调度完成后，会唤醒该进程回到此处，关中断
    wait_current_del(&(sem->wait_queue), wait);//将该进程移出阻塞队列
    local_intr_restore(intr_flag);

    if (wait->wakeup_flags != wait_state) {
        return wait->wakeup_flags;
    }
    return 0;
}
```

##### up()：临界资源的V操作

如果没有进程等待，信号量+1；否则唤醒等待的进程

```c
//临界资源的V操作
static __noinline void __up(semaphore_t *sem, uint32_t wait_state) {
    bool intr_flag;
    local_intr_save(intr_flag);//关中断
    {
        wait_t *wait;
        if ((wait = wait_queue_first(&(sem->wait_queue))) == NULL) {//没有进程等待
            sem->value ++;
        }
        else {
            assert(wait->proc->wait_state == wait_state);
            wakeup_wait(&(sem->wait_queue), wait, wait_state, 1);//有进程等待，唤醒该进程
        }
    }
    local_intr_restore(intr_flag);
}
```

##### phi_test_sema(i)：测试接下来哲学家i是否可以就餐

PV包夹了test函数，该函数测试哲学家i是否能够就餐

- 如果哲学家i此时没在就餐，并且左右哲学家都没就餐，则设置i的状态为可以就餐，并通过`up(&s[i])`使得该函数退出后不会被阻塞在phi_take_forks_sema()的`down(&s[i])`处，接下来可以就餐
- 如果i当前不能就餐，该函数退出后直接被阻塞在`down(&s[i])`处，接下来无法就餐

```c
void phi_test_sema(i) /* i：哲学家号码从0到N-1 */
{ 
    if(state_sema[i]==HUNGRY&&state_sema[LEFT]!=EATING&&state_sema[RIGHT]!=EATING)
    {
        state_sema[i]=EATING;
        up(&s[i]);//抵消phi_take_forks_sema中的down(&s[i])，不会卡在P操作
    }
}
```

最后，哲学家i就餐完毕，使用phi_put_forks_sema(i)放下两边的叉子，并检查左右邻居是否能够就餐，如果可以，则邻居的s信号量++，就不会被卡在phi_take_forks_sema()的down(&s[i])处了，可以开始就餐

```c
void phi_put_forks_sema(int i) /* i：哲学家号码从0到N-1 */
{ 
        down(&mutex); /* 进入临界区 */
        state_sema[i]=THINKING; /* 哲学家进餐结束 */
        phi_test_sema(LEFT); /* 看一下左邻居现在是否能进餐 */
        phi_test_sema(RIGHT); /* 看一下右邻居现在是否能进餐 */
        up(&mutex); /* 离开临界区 */
}
```

##### 总结：哲学家就餐伪代码

```python
philosopher_using_semaphore(i):
    思考
    phi_take_forks_sema(i):
        phi_test_sema(i):
            if i可以就餐:
                V(s[i])
        P(s[i])
    就餐
    phi_put_forks_sema(i):
        phi_test_sema(LEFT(i)):
            if LEFT(i)可以就餐:
                V(s[LEFT(i)])
        phi_test_sema(RIGHT(i)):
            if RIGHT(i)可以就餐:
                V(s[RIGHT(i)])
```

#### 练习2: 基于管程和内核级条件变量的哲学家就餐问题

由于信号量机制编程复杂，引入管程将 对共享资源的所有访问及其所需要的同步操作 集中并封装起来，不直接操作信号量。

并且设置同时只能有一个进程进入管程，从lab7代码来看就是take和put函数完全由管程锁夹住

为了控制进程的执行顺序，给每个管程设置若干条件变量cv，每个cv维护一个阻塞队列，通过wait和signal两种操作来实现入队（挂起）和出队（唤醒）

##### 管程monitor

monitor.h中定义了管程的结构

- mutex：信号量，控制每次只允许一个进程进入管程
- next：信号量，调用signal_cv的进程A会卡在next信号量上
- next_count：由于发出singal_cv而睡眠的进程个数
- cv：条件变量。对于cv有两个核心操作，一个是wait_cv，另一个是signal_cv

```c
typedef struct monitor{
    semaphore_t mutex;      // the mutex lock for going into the routines in monitor, should be initialized to 1
    semaphore_t next;       // the next semaphore is used to down the signaling proc itself, and the other OR wakeuped waiting proc should wake up the sleeped signaling proc.
    int next_count;         // the number of of sleeped signaling proc
    condvar_t *cv;          // the condvars in monitor
} monitor_t;
```

##### 条件变量condvar

- sem：信号量，调用wait_cv的进程会卡在sem信号量上
- count：调用wait_cv，因等待cv成立而进入睡眠的进程个数
- owner：条件变量属于哪个管程

```c
typedef struct condvar{
    semaphore_t sem;        // the sem semaphore  is used to down the waiting proc, and the signaling proc should up the waiting proc
    int count;              // the number of waiters on condvar
    monitor_t * owner;      // the owner(monitor) of this condvar
} condvar_t;
```

check_sync()的后半部分即用管程来解决哲学家就餐问题，可以发现和使用信号量的差别仅在于调用了philosopher_using_condvar()函数

```c
void check_sync(void){
    int i;
    //check condition variable
    monitor_init(&mt, N);//初始化管程
    for(i=0;i<N;i++){
        state_condvar[i]=THINKING;
        int pid = kernel_thread(philosopher_using_condvar, (void *)i, 0);//入口
        if (pid <= 0) {
            panic("create No.%d philosopher_using_condvar failed.\n");
        }
        philosopher_proc_condvar[i] = find_proc(pid);
        set_proc_name(philosopher_proc_condvar[i], "philosopher_condvar_proc");
    }
}
```

条件变量版的philosopher_using_condvar函数也没有本质区别，主要是拿起和放下叉子用了phi_take_forks_condvar()和phi_put_forks_condvar()

```c
//---------- philosophers using monitor (condition variable) ----------------------
int philosopher_using_condvar(void * arg) { /* arg is the No. of philosopher 0~N-1*/
  
    int i, iter=0;
    i=(int)arg;
    cprintf("I am No.%d philosopher_condvar\n",i);
    while(iter++<TIMES)
    { /* iterate*/
        cprintf("Iter %d, No.%d philosopher_condvar is thinking\n",iter,i); /* thinking*/
        do_sleep(SLEEP_TIME);
        phi_take_forks_condvar(i); //拿叉子
        /* need two forks, maybe blocked */
        cprintf("Iter %d, No.%d philosopher_condvar is eating\n",iter,i); /* eating*/
        do_sleep(SLEEP_TIME);
        phi_put_forks_condvar(i); //放下叉子
        /* return two forks back*/
    }
    cprintf("No.%d philosopher_condvar quit\n",i);
    return 0;    
}
```

##### phi_take_forks_condvar()：拿起叉子

take函数完全由管程锁夹住

```c
void phi_take_forks_condvar(int i) {
      down(&(mtp->mutex));//P(mutex)，管程锁
      state_condvar[i]=HUNGRY; 
      // try to get fork
      phi_test_condvar(i); //测试i能否拿到叉子
      while (state_condvar[i] != EATING) {//如果不能拿
          cprintf("phi_take_forks_condvar: %d didn't get fork and will wait\n",i);
          cond_wait(&mtp->cv[i]);//wait(cv[i])，等待其他人释放   
      }
//--------leave routine in monitor--------------
      if(mtp->next_count>0)//next队列非空
         up(&(mtp->next));//唤醒一个next队列中的进程
      else
         up(&(mtp->mutex));//V(mutex)
}
```

##### phi_put_forks_condvar()：放下叉子

put函数完全由管程锁夹住。注意每个人用餐完毕后要通知左右邻居，修改他们的状态为EATING

```c
void phi_put_forks_condvar(int i) {
      down(&(mtp->mutex));//P(mutex)，管程锁
      state_condvar[i]=THINKING;
      // test left and right neighbors
      phi_test_condvar(LEFT);//吃完先通知左边的邻居可以拿叉子
      phi_test_condvar(RIGHT);
//--------leave routine in monitor--------------
     if(mtp->next_count>0)
        up(&(mtp->next));
     else
        up(&(mtp->mutex));
}
```

##### phi_test_condvar(i)：测试哲学家i能否拿起叉子

```c
void phi_test_condvar (i) { 
    if(state_condvar[i]==HUNGRY&&state_condvar[LEFT]!=EATING
            &&state_condvar[RIGHT]!=EATING) {
        cprintf("phi_test_condvar: state_condvar[%d] will eating\n",i);
        state_condvar[i] = EATING ;
        cprintf("phi_test_condvar: signal self_cv[%d] \n",i);
        cond_signal(&mtp->cv[i]) ;//signal(cv[i])
    }
}
```

##### wait_cv

cond_wait()即wait_cv操作，被一个进程调用，**等待断言Pc**被满足后，该进程才可恢复执行。进程挂在该条件变量上等待时，不被认为是占用了管程

```c
// Suspend calling thread on a condition variable waiting for condition Atomically unlocks 
// mutex and suspends calling thread on conditional variable after waking up locks mutex. 
// Notice: mp is mutex semaphore for monitor's procedures
void
cond_wait (condvar_t *cvp) {
    //LAB7 EXERCISE1: YOUR CODE
    cprintf("cond_wait begin:  cvp %x, cvp->count %d, cvp->owner->next_count %d\n", cvp, cvp->count, cvp->owner->next_count);
      cvp->count++;//等待cv成立而进入睡眠的进程个数+1
      if(cvp->owner->next_count > 0)//如果被卡在next的进程数大于0
         up(&(cvp->owner->next));//唤醒一个被卡在next的进程,V(next)
      else
         up(&(cvp->owner->mutex));//如果没有被卡在next的进程，V(mutex)
      down(&(cvp->sem));//P(sem),当前进程等待cv，进入睡眠
      cvp->count --;//一旦被唤醒，等待cv的进程数-1
    cprintf("cond_wait end:  cvp %x, cvp->count %d, cvp->owner->next_count %d\n", cvp, cvp->count, cvp->owner->next_count);
}

```

##### signal_cv

cond_signal()即signal_cv操作，被一个进程调用，以**指出断言Pc现在为真**，从而可以唤醒等待断言Pc被满足的进程继续执行。

发出signal_cv的进程A会唤醒睡眠进程B，进程B执行会导致进程A睡眠（next_count++），直到进程B离开管程，进程A才能继续执行（next_count--），这个同步过程是通过信号量next完成的

```c
// Unlock one of threads waiting on the condition variable. 
void 
cond_signal (condvar_t *cvp) {
   //LAB7 EXERCISE1: YOUR CODE
   cprintf("cond_signal begin: cvp %x, cvp->count %d, cvp->owner->next_count %d\n", cvp, cvp->count, cvp->owner->next_count);  
     if(cvp->count>0) {//如果条件变量cv的等待队列不为空
        cvp->owner->next_count ++;//由于发出singal_cv而睡眠的进程个数+1
        up(&(cvp->sem));//V(sem),唤醒等待sem而睡眠的进程
        down(&(cvp->owner->next));//P(next),使得发出singal_cv的当前进程睡眠
        cvp->owner->next_count --;//一旦解除了睡眠（不被卡在down），就将睡眠数-1
      }
   cprintf("cond_signal end: cvp %x, cvp->count %d, cvp->owner->next_count %d\n", cvp, cvp->count, cvp->owner->next_count);
}
```

##### 总结：流程图

<img src="pic/管程.png" style="zoom:80%;" />

相当于有两个阻塞队列sem和next

- 假设A先用餐，A的邻居B此时测试能否拿起叉子，发现不能后进入sem队列
- C发起测试，发现B在sem队列中，此时C唤醒B，而C进入next队列
- A用餐完毕后会测试其左右邻居，将B的状态设为EATING
- B发现可以拿叉子，于是V(next)唤醒C，此时B的地位变为A
- 如果C与B相邻，则C还要进入sem队列，此时C的地位变为B

##### 总结：问题

1.为什么不能只有一个队列sem，还要设计next队列？

​	如果ABC相邻，A用餐后C立即进入并用餐，但B还卡在sem队列，该情况下B会一直饥饿

2.相比完全按照顺序串行用餐，管程有什么好处？

​	可以满足进程并发执行，比如一开始AB不相邻的情况

3.基于管程和基于信号量有什么区别？

​	根据代码可以发现使用管程后，哲学家完全按照来的顺序就餐（FIFO），而基于信号量则是有空叉子即可用餐
