### lab1 系统软件启动过程

每完成一个练习，在lab目录下make qemu查看结果

#### 练习1：生成操作系统镜像文件ucore.img

镜像文件即把许多文件合成一个镜像文件img，之后可以再打开恢复成多个文件

在~moocos/ucore_lab/labcodes/lab1目录执行make V=，可以看到gcc编译了许多c文件，其中sign.c编译后的sign工具规范化bootblock.o，生成主引导扇区。

之后使用dd工具构建5.12MB的ucore.img虚拟磁盘，将bootblock写入img的第一个扇区，将kernel写入第二个扇区之后的部分

观察sign.c，主引导扇区末两个字节是0x55AA，并且大小为512B



##### 操作系统引导过程

- 首先CPU用一条长度跳转指令找到ROM上的BIOS程序
- BIOS进行硬件自检、初始化后，选择一个启动设备（如硬盘、光盘），读取主引导扇区到内存0x7c00处
- CPU跳转到0x7c00执行bootloader，读取磁盘中ELF格式的操作系统到内存



#### 练习2：使用qemu执行并调试lab1中的软件

首先需要在tools/gdbinit写入set architecture i8086，表明执行bootloader程序时，当前的CPU处于Intel 8086的实模式下。

make debug进入gdb后使用`layout regs`查看寄存器值，发现cs=0xf000，eip=0xfff0，因此PC=cs*16+eip=0xffff0，这就是CPU加电后第一条指令的地址。

使用`x /2i 0xffff0`查看该地址处的指令，发现是ljmp指令，该指令中的地址就是BIOS的地址

使用`b *0x7c00`设置断点，再c继续，可以查看主引导程序的汇编



#### 练习3：分析bootloader进入保护模式的过程

##### 实模式与保护模式

Intel 8086是1978年发布的16位CPU，数据总线16位，地址总线20位。CPU执行bootloader程序时就处于Intel 8086的实模式下，实模式的实指的是CPU采用实际物理地址寻址，仅能表示1MB的物理内存空间。

随着CPU的发展，1985年发布了32位CPU——Intel 80386，数据总线和地址总线均为32位。为了向前兼容8086，又要访问更大的内存空间（32位地址就是4G），就需要一个机制从实模式切换为保护模式，使得32位地址线全部有效，并采用分段和分页机制。保护模式指的就是对进程的地址空间进行保护，不像实模式下任一进程都能直接访问其他进程的地址空间

该切换机制依赖于一个硬件逻辑，称为A20 Gate。A20指的是第20位地址线，实模式下A20的控制信号限制为0，这样使得地址最大是2^20-1=0xfffff，越界后又会回滚到0x00000。开启A20地址线后，才可以进入保护模式。

##### 分段机制

分段机制将内存划分成2^14个段，由GDT（全局描述符表Global Descriptor Table）和LDT（局部描述符表）存储逻辑地址到物理地址的映射，GDT实际就是段表。

逻辑地址：[段号|段内偏移]

段表项：[段号|段描述符]

具体分段地址转换的过程为：逻辑地址中的段选择子（包含段号）作为GDT（段表）的索引，找到对应表项中的段描述符（描述段的属性信息），把段描述符中保存的段基址加上段内偏移，形成线性地址。

实际上，段选择子的结构为：[段号|表指示位|请求特权级]

- 表指示位：选择应该访问的段表是全局描述符表（GDT）还是局部描述符表（LDT）
- 请求特权级：0为内核态，3为用户态

#### 练习4：分析bootloader加载ELF格式的OS的过程

##### bootloader如何读取硬盘扇区

0x1f2~0x1f7都是IO访问地址。等待硬盘空闲后，向0x1F2写入1，向0x1F3写入扇区号secno的末8位，向0x1F4写入secno的8~15位，向0x1F5写入secno的16~23位，向0x1F6低4位写入secno的24~27位，高4位写入0xe0，表示从主盘读取，向0x1F7写入0x20。insl函数将扇区数据以4B为单位读到0x1F0

##### bootloader如何加载ELF格式的OS

操作系统内核kernel是ELF格式文件，文件又包含若干扇区

bootmain()中，首先调用readseg()函数从kernel读取8个扇区到ELFHDR，然后根据ELF头部的魔数判断是否是有效的ELF文件

ELF头部有一个programe header数组，描述了每个程序段的位置和长度等，用于加载每个程序段到内存



#### 练习5：实现函数调用堆栈跟踪函数

- 函数调用时，内存压栈顺序为：参数，eip（下一条指令地址，返回地址），ebp（当前栈底）
- 函数执行
- 执行完后，用栈顶的ebp值更新ebp寄存器，用栈顶的eip值更新eip寄存器，函数返回

```c
+---------------------------------+栈底
|         args[2]                 |
+---------------------------------+
|         args[1]                 |
+---------------------------------+
|         args[0]                 |
+---------------------------------+
|         eip                     |
+---------------------------------+
|         ebp                     |
+---------------------------------+栈顶
```



print_stackframe()函数相当于反递归，不断破除函数嵌套的外壳，地址由低向高打印栈的内容

首先根据当前寄存器ebp的值找到当前函数的栈顶指针(ebp)

每趟中，(ebp)+8是第一个参数的位置，打印所有参数，每趟最后更新寄存器eip=((ebp)+4)，ebp=((ebp))，不断向上去找母函数的栈顶位置

```c
void
print_stackframe(void) {
    uint32_t ebp = read_ebp(), eip = read_eip();

    int i, j;
    for (i = 0; ebp != 0 && i < STACKFRAME_DEPTH; i ++) {
        cprintf("ebp:0x%08x eip:0x%08x args:", ebp, eip);
        uint32_t *args = (uint32_t *)ebp + 2;
        for (j = 0; j < 4; j ++) {
            cprintf("0x%08x ", args[j]);
        }
        cprintf("\n");
        print_debuginfo(eip - 1);
        eip = ((uint32_t *)ebp)[1];
        ebp = ((uint32_t *)ebp)[0];
    }
}
```



#### 练习6：完善中断初始化和处理

为了提高CPU利用率，采用中断机制。

中断事件分为

- 外部中断/中断
- 内部中断/异常
- 陷入/软中断：请求系统调用

##### 中断处理过程

1. CPU每执行完都会检查刚才执行过程中中断控制器是否发送了中断请求。一旦有中断请求，CPU就会从总线上读取中断号。如int指令引发中断，系统调用的指令就是int 0x80
2. 根据中断号查找**中段描述符表**IDT（中断向量表），IDT保存了中断类型号到中断处理程序所在段的段号（段选择子）的映射，再从段表中找到对应的段描述符，得到中断处理程序的起始地址（中断向量）
3. CPU根据IDT项中的描述符特权级DPL信息，判断是否发生用户态到内核态的转换，或者一直在内核态。**一旦发生用户态到内核态的转换， int指令会将当前用户态的ss和esp值压栈**，并读取内核态的ss和esp值，将系统当前栈切换成内核栈
4. 之后，CPU需要保存现场，依次将eflags, cs, eip, errorCode压入内核栈
5. 更新cs和eip为中断服务程序的地址，开始执行中断服务程序
6. 中断服务程序完成后，通过iret指令恢复被打断程序的执行。iret指令弹出eflags, cs, eip，更新上下文寄存器
7. 如果有特权级切换，还要弹出ss和esp，切换回用户栈

##### lab1中对中断的代码实现

（1）**初始化中断描述符表IDT**：trap.c的idt_init()内，为每种中断设定中断处理相关信息。

SETGATE(gate, istrap, sel, off, dpl)，五个参数为（idt\[i]，是否为中断，中断处理程序所在段的段选择子，中断处理程序的段内偏移，特权级别）

```c
void
idt_init(void) {
    extern uintptr_t __vectors[];
    int i;
    for (i = 0; i < sizeof(idt) / sizeof(struct gatedesc); i ++) {
        SETGATE(idt[i], 0, GD_KTEXT, __vectors[i], DPL_KERNEL);
    }
    SETGATE(idt[T_SYSCALL], 1, GD_KTEXT, __vectors[T_SYSCALL], DPL_USER);
    lidt(&idt_pd);
}
```

特别地，将系统调用T_SYSCALL作为异常，并设定只能从用户态调用

（2）vector.S文件定义了每个**中断的入口程序**：压栈异常号errorCode（中断则压入0）和中断号，然后跳转到统一的入口__alltraps处

```assembly
# handler
.text
.globl __alltraps
.globl vector0
vector0:
  pushl $0
  pushl $0
  jmp __alltraps
.globl vector1
vector1:
  pushl $0
  pushl $1
  jmp __alltraps
```



（3）__alltraps位于trapentry.S，**保存上下文**。压栈顺序：ds,es,fs,gs以及pushregs中的所有寄存器值，此时esp指向trapframe的首地址，将esp作为trap()的参数入栈，调用trap(tf)

（4）trap()调用trap_dispatch()，根据tf中的中断号来具体**处理每种中断**

（5）处理完成后，__trapret依次pop掉之前入栈的内容，即esp,pushregs,gs,fs,es,ds。**所谓pop就是用栈顶的数据更新寄存器**。之后将esp+8，即跳过不需要用的中断号和errorCode

（6）最后通过iret指令**中断返回**，将eip,cs,eflags,esp,ss pop出来

```assembly
.text
.globl __alltraps
__alltraps:
    # push registers to build a trap frame
    # therefore make the stack look like a struct trapframe
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    pushal

    # load GD_KDATA into %ds and %es to set up data segments for kernel
    movl $GD_KDATA, %eax
    movw %ax, %ds
    movw %ax, %es

    # push %esp to pass a pointer to the trapframe as an argument to trap()
    pushl %esp

    # call trap(tf), where tf=%esp
    call trap

    # pop the pushed stack pointer
    popl %esp

    # return falls through to trapret...
.globl __trapret
__trapret:
    # restore registers from stack
    popal

    # restore %ds, %es, %fs and %gs
    popl %gs
    popl %fs
    popl %es
    popl %ds

    # get rid of the trap number and error code
    addl $0x8, %esp
    iret

```

##### trapframe

trapframe（栈帧，tf，临时栈）相当于现场寄存器的集合体

```c
struct pushregs {
    uint32_t reg_edi;
    uint32_t reg_esi;
    uint32_t reg_ebp;
    uint32_t reg_oesp;            /* Useless */
    uint32_t reg_ebx;
    uint32_t reg_edx;
    uint32_t reg_ecx;
    uint32_t reg_eax;
};
struct trapframe {
    struct pushregs tf_regs;
    uint16_t tf_gs;
    uint16_t tf_padding0;//和上面一行拼接成32位
    uint16_t tf_fs;
    uint16_t tf_padding1;
    uint16_t tf_es;
    uint16_t tf_padding2;
    uint16_t tf_ds;
    uint16_t tf_padding3;
    //####################以上在__alltraps压栈，由__trapret弹栈
    uint32_t tf_trapno;
    /* below here defined by x86 hardware */
    uint32_t tf_err;
    //####################以上在vector压栈
    uintptr_t tf_eip;
    uint16_t tf_cs;
    uint16_t tf_padding4;
    uint32_t tf_eflags;
    /* below here only when crossing rings, such as from user to kernel */
    uintptr_t tf_esp;
    uint16_t tf_ss;
    uint16_t tf_padding5;
    //####################以上由int指令压栈，iret弹栈
} __attribute__((packed));
```



#### 扩展练习1：用户态和内核态切换

用户态和内核态的主要区别就是各种段寄存器的内容，即段号和特权级不同。

比如，代码段寄存器cs的结构是[代码段段号|TI|特权级]，末两位是当前特权级CPL字段，可以直接看出CPU当前处于什么态

准备工作：内核态切换为用户态时，由于产生中断前就是内核态，所以int指令不会压入ss和esp，故中断处理之前需要手动将esp-8，相当于压入两个NULL；用户态切换为内核态则不用

```c
static void
lab1_switch_to_user(void) {
    //LAB1 CHALLENGE 1 : TODO
	asm volatile (
	    "sub $0x8, %%esp \n"
	    "int %0 \n"
	    "movl %%ebp, %%esp"
	    : 
	    : "i"(T_SWITCH_TOU)
	);
}

static void
lab1_switch_to_kernel(void) {
    //LAB1 CHALLENGE 1 :  TODO
	asm volatile (
	    "int %0 \n"
	    "movl %%ebp, %%esp \n"
	    : 
	    : "i"(T_SWITCH_TOK)
	);
}
```

中断处理程序中，需要先将目标态的cs,ds,es写入tf，之后pop时就可以更新相应寄存器

区别：内核态切换到用户态中，由于之前没有给ss和esp赋值，这里进行赋值，使得iret能正确pop

另外，eflags寄存器的12/13位控制IO权限，也需要修改

trap.c

```c
    case T_SWITCH_TOU:
        if (tf->tf_cs != USER_CS) {
            switchk2u = *tf;
            switchk2u.tf_cs = USER_CS;
            switchk2u.tf_ds = switchk2u.tf_es = switchk2u.tf_ss = USER_DS;
            switchk2u.tf_esp = (uint32_t)tf + sizeof(struct trapframe) - 8;
		
            // set eflags, make sure ucore can use io under user mode.
            // if CPL > IOPL, then cpu will generate a general protection.
            switchk2u.tf_eflags |= FL_IOPL_MASK;
		
            // set temporary stack
            // then iret will jump to the right stack
            *((uint32_t *)tf - 1) = (uint32_t)&switchk2u;
        }
        break;
    case T_SWITCH_TOK:
        if (tf->tf_cs != KERNEL_CS) {
            tf->tf_cs = KERNEL_CS;
            tf->tf_ds = tf->tf_es = KERNEL_DS;
            tf->tf_eflags &= ~FL_IOPL_MASK;
            switchu2k = (struct trapframe *)(tf->tf_esp - (sizeof(struct trapframe) - 8));
            memmove(switchu2k, tf, sizeof(struct trapframe) - 8);
            *((uint32_t *)tf - 1) = (uint32_t)switchu2k;
        }
        break;
```



#### 扩展练习2：用键盘实现用户模式内核模式切换

ring 0：内核态；ring 3：用户态

将扩展练习1的内容封装即可

```c
    case IRQ_OFFSET + IRQ_KBD:
        c = cons_getc();
        cprintf("kbd [%03d] %c\n", c, c);
        if(c=='0'){
            switch_to_kernel(tf);
            cprintf("+++ switch to kernel mode +++\n");
        }
        else if(c=='3'){
            switch_to_user(tf);
            cprintf("+++ switch to  user  mode +++\n");
        }
        break;
```
