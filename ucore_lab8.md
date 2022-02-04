### lab8 文件系统

#### 练习1: 完成读文件操作的实现

##### 文件系统初始化

lab8的kern_init()调用了fs_init()来初始化文件系统

```c
void
fs_init(void) {
    vfs_init();//虚拟文件系统初始化
    dev_init();//与文件相关的设备初始化
    sfs_init();//Simple FS文件系统的初始化
}
```

vfs_init()建立了一个双向链表vdev_list

dev_init()调用disk0_device_init，stdin_device_init，stdout_device_init对设备进行初始化，把它们抽象成文件，并建立索引节点inode，最后把它们加入链表vdev_list中，这样可以以文件的形式访问设备

sfs_init()将Simple FS文件系统挂载到虚拟文件系统中。

- 挂载mount：操作系统使一个存储设备的目录和文件可由计算机文件系统访问的过程
- 虚拟文件系统：屏蔽不同文件系统的差异，为用户程序提供统一的接口。并且Linux中一切皆文件，目录、字符设备、块设备、套接字等，都可以以文件的方式被对待

##### 数据结构

lab8的进程控制块proc_struct多了一个成员filesp

```c
struct files_struct *filesp;
```

该files_struct结构体存储了进程文件相关信息，其中使用一个文件数组fd_array（打开文件表）来存储该进程打开的所有文件的file结构体，该数组的索引就是文件的描述符fd

```c
struct files_struct {
    struct inode *pwd;      // inode of present working directory
    struct file *fd_array;  // opened files array
    int files_count;        // the number of opened files
    semaphore_t files_sem;  // lock protect sem
};
```

file结构体

```c
struct file {
    enum {
        FD_NONE, FD_INIT, FD_OPENED, FD_CLOSED,
    } status;
    bool readable;
    bool writable;
    int fd;
    off_t pos;
    struct inode *node;
    int open_count;
};
```

##### 打开文件

假设要读文件"/test/testfile"时，用户进程会产生一个打开文件的系统调用sys_open，进入内核态后调用sysfile_open()函数

```c
int
sysfile_open(const char *__path, uint32_t open_flags) {
    int ret;
    char *path;
    if ((ret = copy_path(&path, __path)) != 0) {
        return ret;
    }
    ret = file_open(path, open_flags);
    kfree(path);
    return ret;
}
```

sysfile_open()调用file_open()，其中

- fd_array_alloc分配一个file结构体给该文件，其中包括了该文件的**文件描述符fd**
- vfs_open调用vfs_lookup**找到文件的inode**
  - vfs_lookup()首先调用get_device()/vfs_get_bootfs()找到根目录"/"的inode
  - 然后通过vop_lookup()（即sfs_inode.c中的sfs_lookup()）来递归找到"/test/testfile"的inode
  - sfs_lookup()调用sfs_lookup_once()，进一步调用sfs_dirent_search_nolock()在目录中查找与路径名匹配的目录项，目录项内是该路径的inode的地址
- vfs_open调用vop_open打开文件

执行语句file->node = node来将当前进程的fd_array[fd]的node指针指向最终得到的inode，**建立了当前进程和file和inode的联系**

最后返回fd

```c
// open file
int
file_open(char *path, uint32_t open_flags) {
    bool readable = 0, writable = 0;
    switch (open_flags & O_ACCMODE) {
    case O_RDONLY: readable = 1; break;
    case O_WRONLY: writable = 1; break;
    case O_RDWR:
        readable = writable = 1;
        break;
    default:
        return -E_INVAL;
    }

    int ret;
    struct file *file;
    if ((ret = fd_array_alloc(NO_FD, &file)) != 0) {//分配一个file结构体
        return ret;
    }

    struct inode *node;
    if ((ret = vfs_open(path, open_flags, &node)) != 0) {//找到文件的inode
        fd_array_free(file);
        return ret;
    }

    file->pos = 0;
    if (open_flags & O_APPEND) {
        struct stat __stat, *stat = &__stat;
        if ((ret = vop_fstat(node, stat)) != 0) {
            vfs_close(node);
            fd_array_free(file);
            return ret;
        }
        file->pos = stat->st_size;
    }

    file->node = node;//建立file和inode的联系
    file->readable = readable;
    file->writable = writable;
    fd_array_open(file);
    return file->fd;//返回文件描述符
}
```

##### 读文件

当要读写文件时，会调用sfs_io_nolock()函数

由于文件一般不是按块对齐，所以分成三个部分来读取，即第一块，中间部分和最后一块

对于每一块，通过sfs_bmap_load_nolock()函数来根据inode内存储的文件逻辑块号找到磁盘块号ino

```c
//若write=0，将文件从offset处开始alenp个字节到缓冲区buf中；若write=1，将buf中alenp长度内容写到文件offset处
static int
sfs_io_nolock(struct sfs_fs *sfs, struct sfs_inode *sin, void *buf, off_t offset, size_t *alenp, bool write) {
    ...
    //如果第一块不是完整的一块，读第一块
    if ((blkoff = offset % SFS_BLKSIZE) != 0) {
        size = (nblks != 0) ? (SFS_BLKSIZE - blkoff) : (endpos - offset);
        //根据inode内存储的文件逻辑块号找到磁盘块号ino
        if ((ret = sfs_bmap_load_nolock(sfs, sin, blkno, &ino)) != 0) {
            goto out;
        }
        if ((ret = sfs_buf_op(sfs, buf, size, ino, blkoff)) != 0) {
            goto out;
        }
        alen += size;
        if (nblks == 0) {
            goto out;
        }
        buf += size, blkno ++, nblks --;
    }
    //将中间数据按块大小size读取
    size = SFS_BLKSIZE;
    while (nblks != 0) {
        if ((ret = sfs_bmap_load_nolock(sfs, sin, blkno, &ino)) != 0) {
            goto out;
        }
        if ((ret = sfs_block_op(sfs, buf, ino, 1)) != 0) {
            goto out;
        }
        alen += size, buf += size, blkno ++, nblks --;
    }
    //读取最后部分
    if ((size = endpos % SFS_BLKSIZE) != 0) {
        if ((ret = sfs_bmap_load_nolock(sfs, sin, blkno, &ino)) != 0) {
            goto out;
        }
        if ((ret = sfs_buf_op(sfs, buf, size, ino, 0)) != 0) {
            goto out;
        }
        alen += size;
    }
    ...
}
```

##### 管道Pipe

管道是Unix的一种进程间通信方式。读进程和写进程共享一个管道文件pipe，写进程使用write系统调用时会将标准输出的数据输出到pipe文件，读进程使用read系统调用从pipe文件读数据

`command 1 | command 2`可以将命令1的标准输出作为命令2的标准输入，最后控制台打印命令2的标准输出

#### 练习2: 完成基于文件系统的执行程序机制的实现

lab5中曾在proc.c中编写load_icode()函数加载elf文件中的程序到用户空间进而执行程序，但当时是从内存中读取elf文件，lab8是从磁盘读取文件

```c
static int
load_icode(int fd, int argc, char **kargv) {
    /* LAB8:EXERCISE2 YOUR CODE  HINT:how to load the file with handler fd  in to process's memory? how to setup argc/argv?
     * MACROs or Functions:
     *  mm_create        - create a mm
     *  setup_pgdir      - setup pgdir in mm
     *  load_icode_read  - read raw data content of program file
     *  mm_map           - build new vma
     *  pgdir_alloc_page - allocate new memory for  TEXT/DATA/BSS/stack parts
     *  lcr3             - update Page Directory Addr Register -- CR3
     */
	/* (1) create a new mm for current process
     * (2) create a new PDT, and mm->pgdir= kernel virtual addr of PDT
     * (3) copy TEXT/DATA/BSS parts in binary to memory space of process
     *    (3.1) read raw data content in file and resolve elfhdr
     *    (3.2) read raw data content in file and resolve proghdr based on info in elfhdr
     *    (3.3) call mm_map to build vma related to TEXT/DATA
     *    (3.4) callpgdir_alloc_page to allocate page for TEXT/DATA, read contents in file
     *          and copy them into the new allocated pages
     *    (3.5) callpgdir_alloc_page to allocate pages for BSS, memset zero in these pages
     * (4) call mm_map to setup user stack, and put parameters into user stack
     * (5) setup current process's mm, cr3, reset pgidr (using lcr3 MARCO)
     * (6) setup uargc and uargv in user stacks
     * (7) setup trapframe for user environment
     * (8) if up steps failed, you should cleanup the env.
     */
    assert(argc >= 0 && argc <= EXEC_MAX_ARG_NUM);

    if (current->mm != NULL) {//当前进程的mm已释放，才能创建新进程的mm
        panic("load_icode: current->mm must be empty.\n");
    }

    int ret = -E_NO_MEM;
    struct mm_struct *mm;
    if ((mm = mm_create()) == NULL) {//（1）分配mm
        goto bad_mm;
    }
    if (setup_pgdir(mm) != 0) {//（2）建立页目录
        goto bad_pgdir_cleanup_mm;
    }

    struct Page *page;

    struct elfhdr __elf, *elf = &__elf;
    //（3.1）从磁盘上的文件fd读取elf文件头
    if ((ret = load_icode_read(fd, elf, sizeof(struct elfhdr), 0)) != 0) {
        goto bad_elf_cleanup_pgdir;
    }

    if (elf->e_magic != ELF_MAGIC) {//elf文件合法
        ret = -E_INVAL_ELF;
        goto bad_elf_cleanup_pgdir;
    }

    struct proghdr __ph, *ph = &__ph;
    uint32_t vm_flags, perm, phnum;
    
    for (phnum = 0; phnum < elf->e_phnum; phnum ++) {
        //（3.2）根据elf头信息找到各程序段的段首program header
        off_t phoff = elf->e_phoff + sizeof(struct proghdr) * phnum;
        //读取段首
        if ((ret = load_icode_read(fd, ph, sizeof(struct proghdr), phoff)) != 0) {
            goto bad_cleanup_mmap;
        }
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
        vm_flags = 0, perm = PTE_U;
        if (ph->p_flags & ELF_PF_X) vm_flags |= VM_EXEC;//设置段权限
        if (ph->p_flags & ELF_PF_W) vm_flags |= VM_WRITE;
        if (ph->p_flags & ELF_PF_R) vm_flags |= VM_READ;
        if (vm_flags & VM_WRITE) perm |= PTE_W;
        //（3.3）为TEXT/DATA段建立vma
        if ((ret = mm_map(mm, ph->p_va, ph->p_memsz, vm_flags, NULL)) != 0) {
            goto bad_cleanup_mmap;
        }
        off_t offset = ph->p_offset;
        size_t off, size;
        uintptr_t start = ph->p_va, end, la = ROUNDDOWN(start, PGSIZE);

        ret = -E_NO_MEM;

        end = ph->p_va + ph->p_filesz;
        while (start < end) {
            //（3.4）为TEXT/DATA分配物理页
            if ((page = pgdir_alloc_page(mm->pgdir, la, perm)) == NULL) {
                ret = -E_NO_MEM;
                goto bad_cleanup_mmap;
            }
            off = start - la, size = PGSIZE - off, la += PGSIZE;
            if (end < la) {
                size -= la - end;
            }
            //每次读取size大小的块
            if ((ret = load_icode_read(fd, page2kva(page) + off, size, offset)) != 0) {
                goto bad_cleanup_mmap;
            }
            start += size, offset += size;
        }
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
            //（3.5）为BSS段分配物理页
            if ((page = pgdir_alloc_page(mm->pgdir, la, perm)) == NULL) {
                ret = -E_NO_MEM;
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
    sysfile_close(fd);

    vm_flags = VM_READ | VM_WRITE | VM_STACK;
    //（4）分配用户栈空间
    if ((ret = mm_map(mm, USTACKTOP - USTACKSIZE, USTACKSIZE, vm_flags, NULL)) != 0) {
        goto bad_cleanup_mmap;
    }
    assert(pgdir_alloc_page(mm->pgdir, USTACKTOP-PGSIZE , PTE_USER) != NULL);
    assert(pgdir_alloc_page(mm->pgdir, USTACKTOP-2*PGSIZE , PTE_USER) != NULL);
    assert(pgdir_alloc_page(mm->pgdir, USTACKTOP-3*PGSIZE , PTE_USER) != NULL);
    assert(pgdir_alloc_page(mm->pgdir, USTACKTOP-4*PGSIZE , PTE_USER) != NULL);
    //（5）设置当前进程的mm,cr3,重置页目录
    mm_count_inc(mm);
    current->mm = mm;
    current->cr3 = PADDR(mm->pgdir);
    lcr3(PADDR(mm->pgdir));

    //setup argc, argv （6）处理传入参数
    uint32_t argv_size=0, i;
    for (i = 0; i < argc; i ++) {
        argv_size += strnlen(kargv[i],EXEC_MAX_ARG_LEN + 1)+1;
    }

    uintptr_t stacktop = USTACKTOP - (argv_size/sizeof(long)+1)*sizeof(long);
    char** uargv=(char **)(stacktop  - argc * sizeof(char *));
    
    argv_size = 0;
    for (i = 0; i < argc; i ++) {
        uargv[i] = strcpy((char *)(stacktop + argv_size ), kargv[i]);
        argv_size +=  strnlen(kargv[i],EXEC_MAX_ARG_LEN + 1)+1;
    }
    
    stacktop = (uintptr_t)uargv - sizeof(int);
    *(int *)stacktop = argc;
    //（7）设置tf
    struct trapframe *tf = current->tf;
    memset(tf, 0, sizeof(struct trapframe));
    tf->tf_cs = USER_CS;
    tf->tf_ds = tf->tf_es = tf->tf_ss = USER_DS;
    tf->tf_esp = stacktop;
    tf->tf_eip = elf->e_entry;
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

