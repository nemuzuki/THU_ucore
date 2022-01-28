### lab5 用户进程管理

#### 用户进程的虚拟地址空间

一个用户进程有两种虚拟地址空间：

- 一种是内核虚拟地址空间（内核空间），对于所有用户进程，这个范围的虚拟地址都映射到相同的物理内存空间中，即所有用户进程共享内核虚拟地址空间。这是为了当用户进程产生系统调用，进入内核态后，执行的都是同一份放在物理内存中的内核代码
- 另一种是用户虚拟地址空间（用户空间），不同用户进程运行这一段的虚拟地址可能一样，但映射到不同的物理内存空间。这是为了不同用户进程不会非法访问其他进程的用户空间



#### 练习1: 加载应用程序并执行

lab5中，内核线程init_main的任务不再是输出HelloWorld，而是创建了一个内核线程user_main

```c
static int
init_main(void *arg) {
    size_t nr_free_pages_store = nr_free_pages();
    size_t kernel_allocated_store = kallocated();

    int pid = kernel_thread(user_main, NULL, 0);
```

user_main会调用kernel_execve()

```c
static int
user_main(void *arg) {
#ifdef TEST
    KERNEL_EXECVE2(TEST, TESTSTART, TESTSIZE);
#else
    KERNEL_EXECVE(exit);
#endif
    panic("user_main execve failed.\n");
}
```

kernel_execve()**产生一个系统调用**类型的中断T_SYSCALL。查看trap.c的中断分发函数trap_dispatch()，可以看到调用了syscall()，参数arg[]有4个 ：name,len,binary,size

```c
// kernel_execve - do SYS_exec syscall to exec a user program called by user_main kernel_thread
static int
kernel_execve(const char *name, unsigned char *binary, size_t size) {
    int ret, len = strlen(name);
    asm volatile (
        "int %1;"
        : "=a" (ret)
        : "i" (T_SYSCALL), "0" (SYS_exec), "d" (name), "c" (len), "b" (binary), "D" (size)
        : "memory");
    return ret;
}
```

在syscall()中，通过调用sys_exec()，再调用do_execve()函数来执行一个用户进程

```c
static int
sys_exec(uint32_t arg[]) {
    const char *name = (const char *)arg[0];
    size_t len = (size_t)arg[1];
    unsigned char *binary = (unsigned char *)arg[2];
    size_t size = (size_t)arg[3];
    return do_execve(name, len, binary, size);
}
```

##### do_execve()：回收当前进程占用的内存空间，并加载新的进程

```c
int
do_execve(const char *name, size_t len, unsigned char *binary, size_t size) {
    struct mm_struct *mm = current->mm;
    if (!user_mem_check(mm, (uintptr_t)name, len, 0)) {
        return -E_INVAL;
    }
    if (len > PROC_NAME_LEN) {
        len = PROC_NAME_LEN;
    }

    char local_name[PROC_NAME_LEN + 1];
    memset(local_name, 0, sizeof(local_name));
    memcpy(local_name, name, len);

    if (mm != NULL) {
        lcr3(boot_cr3);
        if (mm_count_dec(mm) == 0) {
            exit_mmap(mm);//释放当前进程的用户空间内存，释放页表内存
            put_pgdir(mm);//释放页目录所占内存
            mm_destroy(mm);//释放mm以及其管理的vma
        }
        current->mm = NULL;
    }
    int ret;
    if ((ret = load_icode(binary, size)) != 0) {//加载elf文件，把新的程序加载到用户空间
        goto execve_exit;
    }
    set_proc_name(current, local_name);
    return 0;

execve_exit:
    do_exit(ret);
    panic("already exit: %e.\n", ret);
}
```

##### load_icode()：加载elf文件中的程序到用户空间

包括6个步骤

- 建立mm
- 建立页目录
- 加载elf文件中的各个段到内存
- 分配用户栈空间
- 为当前进程的mm设定参数
- 设定当前进程的trapframe为用户态的对应值，其中
  - 栈顶esp设置为0xB0000000，这正是用户空间的最大虚拟地址
  - eip设置为elf->e_entry，即该elf文件对应的程序入口地址，使得系统调用中断返回时能够跳转到该地址执行elf文件的程序

```c
static int
load_icode(unsigned char *binary, size_t size) {
    if (current->mm != NULL) {
        panic("load_icode: current->mm must be empty.\n");
    }

    int ret = -E_NO_MEM;
    struct mm_struct *mm;
    //(1) create a new mm for current process
    if ((mm = mm_create()) == NULL) {
        goto bad_mm;
    }
    //(2) create a new PDT, and mm->pgdir= kernel virtual addr of PDT
    if (setup_pgdir(mm) != 0) {
        goto bad_pgdir_cleanup_mm;
    }
    //(3) copy TEXT/DATA section, build BSS parts in binary to memory space of process
    struct Page *page;
    //(3.1) get the file header of the bianry program (ELF format)
    struct elfhdr *elf = (struct elfhdr *)binary;
    //(3.2) get the entry of the program section headers of the bianry program (ELF format)
    struct proghdr *ph = (struct proghdr *)(binary + elf->e_phoff);
    //(3.3) This program is valid?
    if (elf->e_magic != ELF_MAGIC) {
        ret = -E_INVAL_ELF;
        goto bad_elf_cleanup_pgdir;
    }

    uint32_t vm_flags, perm;
    struct proghdr *ph_end = ph + elf->e_phnum;
    for (; ph < ph_end; ph ++) {//遍历每个段
    //(3.4) find every program section headers
        if (ph->p_type != ELF_PT_LOAD) {
            continue ;
        }
        if (ph->p_filesz > ph->p_memsz) {
            ret = -E_INVAL_ELF;
            goto bad_cleanup_mmap;
        }
        if (ph->p_filesz == 0) {
            continue ;
        }
    //(3.5) call mm_map fun to setup the new vma ( ph->p_va, ph->p_memsz)
        vm_flags = 0, perm = PTE_U;
        if (ph->p_flags & ELF_PF_X) vm_flags |= VM_EXEC;
        if (ph->p_flags & ELF_PF_W) vm_flags |= VM_WRITE;
        if (ph->p_flags & ELF_PF_R) vm_flags |= VM_READ;
        if (vm_flags & VM_WRITE) perm |= PTE_W;
        if ((ret = mm_map(mm, ph->p_va, ph->p_memsz, vm_flags, NULL)) != 0) {
            goto bad_cleanup_mmap;
        }
        unsigned char *from = binary + ph->p_offset;
        size_t off, size;
        uintptr_t start = ph->p_va, end, la = ROUNDDOWN(start, PGSIZE);

        ret = -E_NO_MEM;

     //(3.6) alloc memory, and  copy the contents of every program section (from, from+end) to process's memory (la, la+end)
        end = ph->p_va + ph->p_filesz;
     //(3.6.1) copy TEXT/DATA section of bianry program
        while (start < end) {
            if ((page = pgdir_alloc_page(mm->pgdir, la, perm)) == NULL) {
                goto bad_cleanup_mmap;
            }
            off = start - la, size = PGSIZE - off, la += PGSIZE;
            if (end < la) {
                size -= la - end;
            }
            memcpy(page2kva(page) + off, from, size);
            start += size, from += size;
        }

      //(3.6.2) build BSS section of binary program
        end = ph->p_va + ph->p_memsz;
        if (start < la) {
            /* ph->p_memsz == ph->p_filesz */
            if (start == end) {
                continue ;
            }
            off = start + PGSIZE - la, size = PGSIZE - off;
            if (end < la) {
                size -= la - end;
            }
            memset(page2kva(page) + off, 0, size);
            start += size;
            assert((end < la && start == end) || (end >= la && start == la));
        }
        while (start < end) {
            if ((page = pgdir_alloc_page(mm->pgdir, la, perm)) == NULL) {
                goto bad_cleanup_mmap;
            }
            off = start - la, size = PGSIZE - off, la += PGSIZE;
            if (end < la) {
                size -= la - end;
            }
            memset(page2kva(page) + off, 0, size);
            start += size;
        }
    }
    //(4) build user stack memory
    vm_flags = VM_READ | VM_WRITE | VM_STACK;
    if ((ret = mm_map(mm, USTACKTOP - USTACKSIZE, USTACKSIZE, vm_flags, NULL)) != 0) {
        goto bad_cleanup_mmap;
    }
    assert(pgdir_alloc_page(mm->pgdir, USTACKTOP-PGSIZE , PTE_USER) != NULL);
    assert(pgdir_alloc_page(mm->pgdir, USTACKTOP-2*PGSIZE , PTE_USER) != NULL);
    assert(pgdir_alloc_page(mm->pgdir, USTACKTOP-3*PGSIZE , PTE_USER) != NULL);
    assert(pgdir_alloc_page(mm->pgdir, USTACKTOP-4*PGSIZE , PTE_USER) != NULL);
    
    //(5) set current process's mm, sr3, and set CR3 reg = physical addr of Page Directory
    mm_count_inc(mm);
    current->mm = mm;
    current->cr3 = PADDR(mm->pgdir);
    lcr3(PADDR(mm->pgdir));

    //(6) setup trapframe for user environment
    struct trapframe *tf = current->tf;
    memset(tf, 0, sizeof(struct trapframe));
    /* LAB5:EXERCISE1 YOUR CODE
     * should set tf_cs,tf_ds,tf_es,tf_ss,tf_esp,tf_eip,tf_eflags
     * NOTICE: If we set trapframe correctly, then the user level process can return to USER MODE from kernel. So
     *          tf_cs should be USER_CS segment (see memlayout.h)
     *          tf_ds=tf_es=tf_ss should be USER_DS segment
     *          tf_esp should be the top addr of user stack (USTACKTOP)
     *          tf_eip should be the entry point of this binary program (elf->e_entry)
     *          tf_eflags should be set to enable computer to produce Interrupt
     */
    tf->tf_cs = USER_CS;
    tf->tf_ds = tf->tf_es = tf->tf_ss = USER_DS;
    tf->tf_esp = USTACKTOP;//0xB0000000
    tf->tf_eip = elf->e_entry;//中断返回地址为elf对应的程序入口地址
    tf->tf_eflags = FL_IF;
    ret = 0;
out:
    return ret;
bad_cleanup_mmap:
    exit_mmap(mm);
bad_elf_cleanup_pgdir:
    put_pgdir(mm);
bad_pgdir_cleanup_mm:
    mm_destroy(mm);
bad_mm:
    goto out;
}

```

##### 总结：如何执行一个用户进程

- 产生一个执行用户程序的sys_exec()系统调用中断
- 回收当前进程占用的内存空间
- 加载新进程的elf文件，建立用户进程运行的环境和数据结构，tf中设置上下文
- 中断返回时跳转到用户程序入口地址执行



#### 练习2: 父进程复制自己的内存空间给子进程

lab4中，do_fork()函数给子进程分配了资源，包括内核栈和上下文等。但由于lab4只对内核线程管理，而内核线程共享虚拟地址空间，即不同内核线程的虚拟地址映射到同一物理地址，所以不需要虚拟内存管理的数据结构mm来帮助换页等任务

而lab5中用户进程有自己的用户虚拟地址空间，所以需要调用copy_mm()函数来为新进程创建mm

参数clone_flags用来选择子进程是否共享父进程的地址空间

- 如果可以，则直接复制父进程的mm
- 否则要
  - mm_create()创建新的mm
  - setup_pgdir()分配页目录
  - dup_mmap()复制父进程的内存内容给子进程

proc.c

```c
static int
copy_mm(uint32_t clone_flags, struct proc_struct *proc) {
    struct mm_struct *mm, *oldmm = current->mm;

    /* current is a kernel thread */
    if (oldmm == NULL) {
        return 0;
    }
    if (clone_flags & CLONE_VM) {//可以共享地址空间，则复制mm
        mm = oldmm;
        goto good_mm;
    }

    int ret = -E_NO_MEM;
    if ((mm = mm_create()) == NULL) {//如果不可以共享地址空间，创建新的mm
        goto bad_mm;
    }
    if (setup_pgdir(mm) != 0) {//分配页目录
        goto bad_pgdir_cleanup_mm;
    }

    lock_mm(oldmm);//上互斥锁
    {
        ret = dup_mmap(mm, oldmm);//复制内存空间内容
    }
    unlock_mm(oldmm);

    if (ret != 0) {
        goto bad_dup_cleanup_mmap;
    }

good_mm:
    mm_count_inc(mm);//共享地址空间的进程数++
    proc->mm = mm;
    proc->cr3 = PADDR(mm->pgdir);//设定页目录基址
    return 0;
bad_dup_cleanup_mmap:
    exit_mmap(mm);
    put_pgdir(mm);
bad_pgdir_cleanup_mm:
    mm_destroy(mm);
bad_mm:
    return ret;
}
```

其中，dup_mmap()遍历父进程的每一块虚拟地址空间，并调用copy_range()来复制父进程内容到子进程的地址空间

vmm.c

```c
int
dup_mmap(struct mm_struct *to, struct mm_struct *from) {
    assert(to != NULL && from != NULL);
    list_entry_t *list = &(from->mmap_list), *le = list;
    while ((le = list_prev(le)) != list) {
        struct vma_struct *vma, *nvma;
        vma = le2vma(le, list_link);
        nvma = vma_create(vma->vm_start, vma->vm_end, vma->vm_flags);
        if (nvma == NULL) {
            return -E_NO_MEM;
        }

        insert_vma_struct(to, nvma);

        bool share = 0;
        if (copy_range(to->pgdir, from->pgdir, vma->vm_start, vma->vm_end, share) != 0) {
            return -E_NO_MEM;
        }
    }
    return 0;
}
```

##### copy_range()：复制父进程内容到子进程的地址空间

遍历父进程的每个物理页，复制内容给子进程的物理页，并建立子进程虚拟地址到物理地址的映射

pmm.c（这里存疑，要复制的应该是用户虚拟地址空间的物理页，所以要找用户虚拟地址，为何page2kva找内核虚拟地址？）

```c
int
copy_range(pde_t *to, pde_t *from, uintptr_t start, uintptr_t end, bool share) {
    assert(start % PGSIZE == 0 && end % PGSIZE == 0);
    assert(USER_ACCESS(start, end));
    // copy content by page unit.
    do {
        //call get_pte to find process A's pte according to the addr start
        pte_t *ptep = get_pte(from, start, 0), *nptep;
        if (ptep == NULL) {
            start = ROUNDDOWN(start + PTSIZE, PTSIZE);
            continue ;
        }
        //call get_pte to find process B's pte according to the addr start. If pte is NULL, just alloc a PT
        if (*ptep & PTE_P) {
            if ((nptep = get_pte(to, start, 1)) == NULL) {
                return -E_NO_MEM;
            }
            uint32_t perm = (*ptep & PTE_USER);
            //get page from ptep
            struct Page *page = pte2page(*ptep);//父进程的物理页
            // alloc a page for process B
            struct Page *npage=alloc_page();
            assert(page!=NULL);
            assert(npage!=NULL);
            int ret=0;
            
            void * kva_src = page2kva(page);//父进程的物理页对应的内核虚拟地址
            void * kva_dst = page2kva(npage);//子进程的物理页对应的内核虚拟地址
        
            memcpy(kva_dst, kva_src, PGSIZE);//复制内容

            ret = page_insert(to, npage, start, perm);//建立子进程内核虚拟地址到物理地址的映射
            assert(ret == 0);
        }
        start += PGSIZE;
    } while (start != 0 && start < end);//不断遍历父进程的物理页
    return 0;
}
```

##### Copy on Write机制

操作系统给子进程分配资源时，父进程不会把整个内存内容复制给子进程，如果二者要读同一个资源则共享该资源，只有当子进程要写物理页时，才会复制一个副本，在副本中修改

实现方法：子进程的虚拟地址和父进程的默认映射到相同的物理页，并在页表项中设置该页为不可写的，当一方要写时，则会产生访问异常。中断处理程序再分配新的物理页给子进程，修改页表项



#### 练习3：理解进程执行 fork/exec/wait/exit 的实现，以及系统调用的实现

syscall.c中定义了若干系统调用函数sys_xxx()，分别调用内核函数do_xxx()

```c
static int
sys_exit(uint32_t arg[]) {
    int error_code = (int)arg[0];
    return do_exit(error_code);
}

static int
sys_fork(uint32_t arg[]) {
    struct trapframe *tf = current->tf;
    uintptr_t stack = tf->tf_esp;
    return do_fork(0, stack, tf);
}

static int
sys_wait(uint32_t arg[]) {
    int pid = (int)arg[0];
    int *store = (int *)arg[1];
    return do_wait(pid, store);
}

static int
sys_exec(uint32_t arg[]) {
    const char *name = (const char *)arg[0];
    size_t len = (size_t)arg[1];
    unsigned char *binary = (unsigned char *)arg[2];
    size_t size = (size_t)arg[3];
    return do_execve(name, len, binary, size);
}
```

do_execve()：回收当前进程占用的内存空间，并加载新的进程，练习1已经描述过。

##### do_fork()：给子进程分配资源，并建立进程间联系

lab5中，进程控制块proc_struct多了一些成员变量

- cptr：children，指向当前进程的最年轻的子进程
- yptr：younger sibling，指向年轻的兄弟进程
- optr：older sibling，指向年长的兄弟进程

```c
struct proc_struct {
    ...
    int exit_code;                              // exit code (be sent to parent proc)
    uint32_t wait_state;                        // waiting state
    struct proc_struct *cptr, *yptr, *optr;     // relations between processes
};
```

lab5中do_fork()不再简单地增加进程数量，而是调用set_links()函数建立了进程间的联系

```c
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        proc->pid = get_pid();
        hash_proc(proc);
        set_links(proc);//建立了进程间的联系
    }
    local_intr_restore(intr_flag);
```

set_links()

```c
// set_links - set the relation links of process
static void
set_links(struct proc_struct *proc) {
    list_add(&proc_list, &(proc->list_link));
    proc->yptr = NULL;
    if ((proc->optr = proc->parent->cptr) != NULL) {//进程父亲已经存在儿子
        proc->optr->yptr = proc;//进程的哥哥的弟弟是自己
    }
    proc->parent->cptr = proc;//进程父亲的儿子是自己
    nr_process ++;
}
```



##### do_wait()：让子进程执行直到结束，再执行父进程

首先引入**僵尸进程**的概念：如果子进程比父进程先结束，而父进程又没有释放子进程占用的资源，此时子进程将成为一个僵尸进程

do_wait()函数先检查当前进程的子进程是否是僵尸进程，如果是的话就释放其所有资源，包括

- 将子进程移出hash链表和进程链表
- 释放子进程的内核栈
- 释放子进程的进程控制块

如果子进程不是僵尸进程，则设定父进程的状态为阻塞态，然后CPU调度执行子进程，等待子进程执行完毕再执行

```c
//当前进程检查pid进程是否是其子进程，如果是的话等子进程执行完再执行父进程
int
do_wait(int pid, int *code_store) {
    struct mm_struct *mm = current->mm;
    if (code_store != NULL) {
        if (!user_mem_check(mm, (uintptr_t)code_store, sizeof(int), 1)) {
            return -E_INVAL;
        }
    }

    struct proc_struct *proc;
    bool intr_flag, haskid;
repeat:
    haskid = 0;
    if (pid != 0) {
        proc = find_proc(pid);
        if (proc != NULL && proc->parent == current) {//pid进程是当前进程的子进程
            haskid = 1;//当前进程有子进程
            if (proc->state == PROC_ZOMBIE) {//如果子进程是僵尸进程
                goto found;
            }
        }
    }
    else {//pid进程是0号进程
        proc = current->cptr;//当前进程的最年轻的子进程
        for (; proc != NULL; proc = proc->optr) {//遍历当前进程的所有子进程
            haskid = 1;
            if (proc->state == PROC_ZOMBIE) {//如果子进程是僵尸进程
                goto found;
            }
        }
    }
    if (haskid) {//当前进程有子进程，且不是僵尸进程
        current->state = PROC_SLEEPING;//设定当前进程状态为阻塞态
        current->wait_state = WT_CHILD;//设定当前进程等待子进程执行
        schedule();//让CPU调度执行子进程
        if (current->flags & PF_EXITING) {
            do_exit(-E_KILLED);
        }
        goto repeat;
    }
    return -E_BAD_PROC;

found://子进程是僵尸进程时，释放子进程的所有资源
    if (proc == idleproc || proc == initproc) {
        panic("wait idleproc or initproc.\n");
    }
    if (code_store != NULL) {
        *code_store = proc->exit_code;
    }
    local_intr_save(intr_flag);
    {
        unhash_proc(proc);//将子进程移出hash链表
        remove_links(proc);//将子进程移出进程链表
    }
    local_intr_restore(intr_flag);
    put_kstack(proc);//释放子进程的内核栈
    kfree(proc);//释放子进程的进程控制块
    return 0;
}
```

##### do_exit()：退出当前进程，并将子进程交给init进程托管

首先释放当前进程的资源

其次，还要处理进程间的关系

- 如果当前进程的父进程在等当前进程，则唤醒父进程变为就绪态
- 如果当前进程有子进程，则将子进程加入init进程的子进程链表

```c
int
do_exit(int error_code) {
    if (current == idleproc) {
        panic("idleproc exit.\n");
    }
    if (current == initproc) {
        panic("initproc exit.\n");
    }
    
    struct mm_struct *mm = current->mm;
    if (mm != NULL) {
        lcr3(boot_cr3);
        if (mm_count_dec(mm) == 0) {//如果mm管理的内存没有进程使用，则释放内存三件套
            exit_mmap(mm);
            put_pgdir(mm);
            mm_destroy(mm);
        }
        current->mm = NULL;
    }
    current->state = PROC_ZOMBIE;//当前进程变为僵尸进程
    current->exit_code = error_code;
    
    bool intr_flag;
    struct proc_struct *proc;
    local_intr_save(intr_flag);
    {
        proc = current->parent;
        if (proc->wait_state == WT_CHILD) {//如果当前进程的父进程在等当前进程，则唤醒父进程变为就绪态
            wakeup_proc(proc);
        }
        while (current->cptr != NULL) {//如果当前进程有子进程
            proc = current->cptr;
            current->cptr = proc->optr;
    
            proc->yptr = NULL;
            if ((proc->optr = initproc->cptr) != NULL) {//将子进程加入init进程的子进程链表
                initproc->cptr->yptr = proc;
            }
            proc->parent = initproc;//子进程的父亲设为init进程
            initproc->cptr = proc;//子进程设为init进程的最年轻的子进程
            if (proc->state == PROC_ZOMBIE) {//如果子进程是僵尸进程
                if (initproc->wait_state == WT_CHILD) {
                    wakeup_proc(initproc);
                }
            }
        }
    }
    local_intr_restore(intr_flag);
    
    schedule();
    panic("do_exit will not return!! %d.\n", current->pid);
}
```

需要注意的是，如果当前进程的子进程是僵尸进程，需要唤醒init进程来完成回收该子进程的任务（41-43行）。回顾init_main()，里面会调用do_wait()，而这一函数中处理了子进程为僵尸进程的问题。

所以，只要当前进程退出，则把子进程交给init进程接管，僵尸进程的问题也就不复存在

```c
static int
init_main(void *arg) {
    size_t nr_free_pages_store = nr_free_pages();
    size_t kernel_allocated_store = kallocated();

    int pid = kernel_thread(user_main, NULL, 0);
    if (pid <= 0) {
        panic("create user_main failed.\n");
    }

    while (do_wait(0, NULL) == 0) {
        schedule();
    }
    return 0;
}
```

##### 僵尸进程已回收的和未回收的

在do_exit()中，僵尸进程已经被回收的部分：

```c
exit_mmap(mm);//释放当前进程的用户空间内存，释放页表内存
put_pgdir(mm);//释放页目录所占内存
mm_destroy(mm);//释放mm以及其管理的vma
```

但还需要经由父进程调用do_wait()来回收的部分：

- 将子进程移出hash链表和进程链表
- 释放子进程的内核栈
- 释放子进程的进程控制块

##### 进程状态转换图

![](pic/进程状态转换图.png)