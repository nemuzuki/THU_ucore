static int 
_clock_map_swappable(struct mm_struct *mm, uintptr_t addr, struct Page *page, int swap_in){
    list_entry_t *head=(list_entry_t*) mm->sm_priv;
    list_entry_t *entry=&(page->pra_page_link);
    assert(entry != NULL && head != NULL);

    list_add(head,entry);
    page->clock_flags=0b11;//访问位=修改位=1
    return 0;
}

static int
_clock_swap_out_victim(struct mm_struct *mm, struct Page ** ptr_page, int in_tick)
{
    list_entry_t *head=(list_entry_t*) mm->sm_priv;//用于页面置换的链表的表头
    assert(head != NULL);
    assert(in_tick==0);
    list_entry_t *le=head;
    while(1){
        if(le==&pra_list_head){//头结点不包含page，跳过
            le=list_prev(le);
        }
        struct Page *page=le2page(le,pra_page_link);
        if((page->clock_flags&2)==2){//访问位为1，10或11
            page->clock_flags-=2;
        }
        else if(page->clock_flags==0b01){//01
            page->clock_flags=0;
        }
        else{//00，换出
            *ptr_page=page;
            mm->sm_priv=list_prev(le);
            list_del(le);
            break;
        }
        le=list_prev(le);
    }
    return 0;
}