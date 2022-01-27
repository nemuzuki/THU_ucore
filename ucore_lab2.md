### lab2 物理内存管理

#### 练习1：实现 first-fit 连续物理内存分配算法

First-Fit即首次适应算法，空闲分区以地址递增方式链接，分配给进程时顺序查找

练习一需要实现内存管理的三个函数：初始化空闲页，分配空闲页，回收物理页

##### 物理页结构体Page

- ref：映射此物理页的虚拟页个数
- flags：状态位有两位，1.是否被内核保留，2.property是否有效，如果该页空闲，则这两位为01
- property：如果该页是领头页，propert表示连续空闲页数
- page_link：该页对应空闲链表的节点，节点可通过le2page()找到对应的Page结构体（成员地址减去偏移量得到结构体基地址）

memlayout.h

```c
struct Page {
    int ref;// page frame's reference counter
    uint32_t flags;// array of flags that describe the status of the page frame
    unsigned int property;// the num of free block, used in first fit pm manager
    list_entry_t page_link;// free list link，链表节点
};
```

##### 初始化空闲页

首先，新产出的所有空闲页需要由一个双向循环链表free_list链接。

n个连续空闲页的第一页称为base，领头页

```c
//初始化n个空闲页
static void
default_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p ++) {
        assert(PageReserved(p));
        p->flags = 0;
        SetPageProperty(p);//一旦空闲，property有效
        p->property = 0;
        set_page_ref(p, 0);//引用计数清零
        list_add_before(&free_list, &(p->page_link));//加入空闲链表
    }
    nr_free += n;//链表中空闲页数
    //first block
    base->property = n;//连续空闲页数=n
}


```

##### 分配空闲页

遍历循环链表，直到找到页数大于等于n的连续的空闲页，从链表删除

如果页数大于n，需要将剩余的连续页的第一页作为领头页

```c
//给进程分配n个物理页
static struct Page *
default_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > nr_free) {
        return NULL;
    }
    list_entry_t *le, *len;
    le = &free_list;

    while((le=list_next(le)) != &free_list) {//遍历循环链表，去找到大小合适的连续物理页
      struct Page *p = le2page(le, page_link);//领头页
      if(p->property >= n){//有连续的n页可分配
        int i;
        for(i=0;i<n;i++){//修改要分配的连续的n页
          len = list_next(le);
          struct Page *pp = le2page(le, page_link);
          SetPageReserved(pp);
          ClearPageProperty(pp);//一旦被分配，property属性无效
          list_del(le);//从链表删除
          le = len;
        }
        if(p->property>n){//还有剩余的连续空闲页
          (le2page(le,page_link))->property = p->property - n;//新的领头页
        }
        ClearPageProperty(p);
        SetPageReserved(p);
        nr_free -= n;
        return p;
      }
    }
    return NULL;
}
```

##### 回收物理页

default_free_pages()：回收base领头的n个连续物理页，使其变为空闲页

首先需要找到base在链表中插入的正确位置，加入链表，然后判断是否需要两种合并

- 向后合并：如果base+n正好等于下一领头页的地址p，则向后合并

- 向前合并：再从base向前遍历相邻空闲页（如果有），找到base之前的一个领头页，合并base带领的页


```c
//回收base领头的n个连续物理页，变为空闲页
static void
default_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    assert(PageReserved(base));

    list_entry_t *le = &free_list;
    struct Page * p;
    while((le=list_next(le)) != &free_list) {
      p = le2page(le, page_link);//le节点对应的物理页的地址p
      if(p>base){//因为要按地址顺序安排链表，所以需要找到base插入的正确位置
        break;//此时p是base要插入位置的下一节点，也是领头页
      }
    }
    for(p=base;p<base+n;p++){
      list_add_before(le, &(p->page_link));//加入链表
    }
    base->flags = 0;
    set_page_ref(base, 0);
    // ClearPageProperty(base);
    SetPageProperty(base);
    base->property = n;
    
    //向后合并
    p = le2page(le,page_link) ;
    if( base+n == p ){
      base->property += p->property;//base带领的连续页和le带领的合并（向后合并）
      p->property = 0;
    }
    
    //向前合并
    le = list_prev(&(base->page_link));
    p = le2page(le, page_link);
    if(le!=&free_list && p==base-1){//p和base相邻
      while(le!=&free_list){
        if(p->property){//找到领头页
          p->property += base->property;//向前合并
          base->property = 0;
          break;
        }
        le = list_prev(le);//从base不断向前找
        p = le2page(le,page_link);
      }
    }

    nr_free += n;
    return ;
}
```

#### 练习2：实现寻找虚拟地址对应的页表项

在lab2的内存管理中采用了**二级分页机制**，虚拟地址被划分为：[页目录index(10)|页表index(10)|页内偏移(12)]

由于每页4KB，每张页目录和页表都有1K项，因此每项长度为4B=32bit

##### get_pte找页表项

get_pte()：找到虚拟地址la对应的页表项的地址，如果页目录项没有页表基址，则分配一个物理页作为页表。分配物理页时，有三个步骤：

- alloc_page创建page结构体
- 根据page结构体的地址得到物理页的地址pa
- 根据pa得到内核虚地址KADDR(pa)，用于CPU的访问

```c
//pgdir页目录基址，la虚拟地址，create如果缺页是否分配物理页
pte_t *
get_pte(pde_t *pgdir, uintptr_t la, bool create) {
    pde_t *pdep = &pgdir[PDX(la)];//根据页目录index找到对应页目录项内的页表基址
    if (!(*pdep & PTE_P)) {//如果页目录项没有页表基址
        struct Page *page;
        if (!create || (page = alloc_page()) == NULL) {//在这里分配物理页
            return NULL;
        }
        set_page_ref(page, 1);
        uintptr_t pa = page2pa(page);//page对应的物理页地址
        memset(KADDR(pa), 0, PGSIZE);//初始化一个页大小的内存作为页表
        *pdep = pa | PTE_U | PTE_W | PTE_P;//更新页目录项内容为页表基址和标志位
    }
    return &((pte_t *)KADDR(PDE_ADDR(*pdep)))[PTX(la)];//返回页表项
}
```

其中一些重要的地址转换的函数和宏：

- PDX()：获得虚拟地址高10位的页目录index

- PTX()：获得虚拟地址中间10位的页表index

- page2pa(page)：**将page结构体的地址转为物理页真实地址**。将page结构体的地址减去page数组的基地址pages，相当于得到page的数组下标（20位），然后左移12位（因为每页4KB），作为该page结构体对应的物理页地址pa

  由于这样得到的pa的低12位总为0，所以可以用来作为标志字段，如

  - PTE_U=0x001：表项有效
  - PTE_W=0x002：物理页可写
  - PTE_P=0x004：物理页存在

- KADDR()：**将物理地址转为内核虚拟地址**。pa右移12位得到下标，和物理内存最大页数npage比较，若合法，则将pa与内核基址KERNBASE 0xC0000000相加，得到内核虚地址KADDR(pa)。**注：*和memset的参数都是内核虚拟地址**



#### 练习3：释放某虚地址所在的页并取消对应二级页表项的映射

page_remove_pte

```c
//pgdir页目录基址，la虚拟地址，ptep页表项
static inline void
page_remove_pte(pde_t *pgdir, uintptr_t la, pte_t *ptep) {
    if (*ptep & PTE_P) {//页表项非空
        struct Page *page = pte2page(*ptep);//根据页表项找到对应page
        if (page_ref_dec(page) == 0) {//如果引用数为0，则回收该页
            free_page(page);
        }
        *ptep = 0;//页表项清空
        tlb_invalidate(pgdir, la);//使TLB中对应页表项失效
    }
}
```

如何根据页表项找到对应page

pa2page(pa)：将物理地址右移12位得到下标，加上pages数组基址得到page结构体地址

pte2page(pte)：PTE_ADDR将页表项内容的低12位置0，然后进行pa2page

因此，页表项pte右移12位，加上pages基址即可得到page地址。实际上，pte正是物理地址pa



#### ※系统执行中地址映射的四个阶段

> 该小节不属于作业范畴，但需要深刻理解，体会分段分页中虚拟地址、线性地址、物理地址三者之间的映射变化

首先明确，虚拟地址经过分段单元转换为线性地址，线性地址经过分页映射为物理地址

在lab1中，在tools/kernel.ld中可以看到，kernel被链接到0x100000，这就是内核虚拟地址空间的基址。而ucore中实际地址空间也是从0x100000开始的，段地址映射采用对等关系，也就是GDT中每个段的基地址都是0，此时虚拟地址=线性地址=物理地址

```c
ENTRY(kern_init)
    
SECTIONS {
	/* Load the kernel at this address: "." means the current address */
	. = 0x100000;
```



在lab2中，kernel被链接到0xC0100000，即内核虚拟地址空间从0xC0100000开始，而其物理地址空间基址仍然是0x100000

```c
ENTRY(kern_entry)

SECTIONS {
    /* Load the kernel at this address: "." means the current address */
    . = 0xC0100000;
```

##### 第一阶段

bootloader阶段，即从bootloader的start函数（在boot/bootasm.S中）到执行 ucore kernel的kern_entry函数之前，映射方式和lab1没有区别

```c
va=la=pa
```

##### 第二阶段：分段

从kern_entry函数开始，到执行pmm_init()内的enable_paging函数之前。

entry.S

```assembly
.text
.globl kern_entry
kern_entry:
    # reload temperate gdt (second time) to remap all physical memory
    # virtual_addr 0~4G=linear_addr&physical_addr -KERNBASE~4G-KERNBASE 
    lgdt REALLOC(__gdtdesc)# 重新加载GDT，进行新的空间映射
    movl $KERNEL_DS, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    ljmp $KERNEL_CS, $relocated
__gdt:
    SEG_NULL
    SEG_ASM(STA_X | STA_R, - KERNBASE, 0xFFFFFFFF)      # code segment
    SEG_ASM(STA_W, - KERNBASE, 0xFFFFFFFF)              # data segment
__gdtdesc:
    .word 0x17                                          # sizeof(__gdt) - 1
    .long REALLOC(__gdt)
```

由于第一阶段中va=pa不满足va=pa+0xC0000000的条件，所以__gdt中将段的起始地址由0变成了-KERNBASE，kern_entry中重新加载了全局描述符表GDT，进行新的映射。此时只分段还没有分页，于是

```c
va=la+0xC0000000=pa+0xC0000000
```

##### 第三阶段：分页

从enable_paging到gdt_init之前。boot_map_segment()将线性地址按照la=pa+0xC0000000进行映射，enable_paging()开启了分页机制，此时有

```c
va=la+0xC0000000=pa+2*0xC0000000
```

但这种映射是错误的，因为va=pa+0xC0000000的关系被改变了，所以在启动分页机制之前执行如下操作

```c
boot_pgdir[0] = boot_pgdir[PDX(KERNBASE)];
```

这使得页目录的第0项和第PDX(KERNBASE)=0b1100_0000_00项映射到同一个页表，这样一来，访问虚拟地址va时，先减去段基址得到线性地址la=va-0xC0000000，然后经过页目录第0项进行地址映射，即可得到正确的物理地址pa=va-0xC0000000

然而，只有2^22=4MB范围内的虚拟地址（0xC0000000-0xC0400000）对应着页目录的第0项，经过了正确的映射

```c
va=la+0xC0000000=pa+0xC0000000
```

超出4MB范围的虚拟地址仍然使用以下映射，不能进行正确的映射

```c
va=la+0xC0000000=pa+2*0xC0000000
```

但是由于lab2中ucore小于4MB，所以暂时不用考虑其他空间的映射。如果ucore大于4MB，则需要对4MB以外的虚拟地址修改对应的页目录项

##### 第四阶段：取消分段

gdt_init()第三次更新了GDT，将段基址变为0，解除了分段机制。并通过执行boot_pgdir[0] = 0解除了之前页目录项的映射关系。现在有我们期望的映射关系

```c
va=la=pa+0xC0000000
```

总结：实际上，开始不分段，只开启第三阶段开始的分页机制不就完成了目标嘛？我想这是ucore给我们留下一个实现段页式存储管理的机会，通过四个阶段的映射变化来体会段页式映射的全过程

注：ucore中可以使用cprintf结合%p来打印地址

参考：[ucore操作系统实验笔记 - Lab2 - SegmentFault 思否](https://segmentfault.com/a/1190000009450840)



#### 扩展练习1：buddy system（伙伴系统）分配算法

用一个数组longest来存储一棵满二叉树管理内存，每个节点管理的内存页数都是2的整数次幂。设内存总页数为n，则结点数为2n-1，longest[i]代表第i号节点管辖的页数

- 初始化分配器
- 分配n页的内存：从根向下搜索，直到找到大小=fixsize(n)的节点
- 回收offset处的内存块：从下向上更新节点的大小

注：只是本地实现，没结合ucore

```c
#include<stdio.h>
#include<stdlib.h>
using namespace std;

#define IS_POWER_OF_2(i) (i>0 && (i&(i-1))==0)?1:0
#define LEFT(i) 2*i+1
#define RIGHT(i) 2*i+2 
#define PARENT(i) (i-1)/2
#define MAX(i,j) i>j?i:j 
//大于等于n的最小2的幂 
int fixsize(int n){
	for(int i=n;;++i){
		if(IS_POWER_OF_2(i)){
			return i;
		}
	}
}

//buddy是分配器 
struct buddy{
	unsigned size;//内存的总页数n，共2n-1个节点 
	unsigned longest[200];//longest[i]代表第i号节点管辖的页数 
	void print_buddy(){
    	unsigned node_size=size*2;
	    for(int i=0;i<2*size-1;++i){
	        if(IS_POWER_OF_2(i+1)){//二叉树该层已满  
            	node_size/=2;
	            printf("\n");
	        }
	        printf("(%d,%d) ",i,longest[i]);
	    }
	    printf("\n\n");
	}
};

//初始化一个分配器，构建树形结构
buddy *buddy_init(int size){
    struct buddy* self;
    unsigned node_size;
    int i;
    if(!IS_POWER_OF_2(size)){
        return NULL;
    }
    self=(struct buddy*)malloc(2*size*sizeof(unsigned));
    self->size=size;
    node_size=size*2;
    for(i=0;i<2*size-1;++i){
        if(IS_POWER_OF_2(i+1)){//二叉树该层已满 
            node_size/=2;
        }
        self->longest[i]=node_size;
    }
	self->print_buddy(); 
    return self;
}
//分配size大小的内存 
int buddy_alloc(struct buddy*self,int size){
	unsigned index=0,node_size,offset=0;
	if(!IS_POWER_OF_2(size)){
		size=fixsize(size);
	}
	if(self->longest[index]<size){
		return -1;
	}
	//不断向下搜索，直到节点size等于希望值 
	for(node_size=self->size;node_size!=size;node_size/=2){
		if(self->longest[2*index+1]>=size){//左儿子 
			index=LEFT(index);
		}
		else{
			index=RIGHT(index);
		}
	}
	self->longest[index]=0;//找到要分配的块index，标记为已分配 
	
	offset=(index+1)*node_size-self->size;//该块相对内存基址的偏移，即内存地址
    printf("allocate node: %d, size: %d, address: %d\n",index,node_size,offset);
	 
	while(index){//不断向上更新父节点的大小 
		index=PARENT(index);
		self->longest[index]=MAX(self->longest[LEFT(index)],self->longest[RIGHT(index)]);
	}
	self->print_buddy(); 
	return offset;
} 
//回收内存地址为offset处的块 
void buddy_free(struct buddy* self,int offset){
	unsigned node_size,index=0;
	unsigned left_longest,right_longest;
	node_size=1;
	index=offset+self->size-1;//offset对应的节点下标，除以node_size=1被省略 
    for(;self->longest[index];index=PARENT(index)){//从该节点向前找longest为0的节点
        node_size*=2;
        if(index==0)return;
    }
    self->longest[index]=node_size;//恢复原来大小
    printf("free node: %d, size: %d, address: %d\n",index,node_size,offset); 
	 
    while(index){//合并
        index=PARENT(index);
        node_size*=2;
        left_longest=self->longest[LEFT(index)];
        right_longest=self->longest[RIGHT(index)];
        if(left_longest+right_longest==node_size){//左右儿子都满 
            self->longest[index]=node_size;
        }
        else{//左右儿子有空 
            self->longest[index]=MAX(left_longest,right_longest);
        }
    }
	self->print_buddy(); 
} 
int main(){
	buddy *b=buddy_init(16);
	buddy_alloc(b,7);
	buddy_alloc(b,1);
	buddy_alloc(b,1);
    
	buddy_free(b,8);
	buddy_free(b,0);
	buddy_free(b,9);
}
```

