#ifndef __KERNEL_POPCORN_PAGE_SERVER_H__
#define __KERNEL_POPCORN_PAGE_SERVER_H__

int page_server_flush_remote_pages(struct remote_context *rc);

/* Implemented in mm/memory.c */
int handle_pte_fault_origin(struct mm_struct *, struct vm_area_struct *, unsigned long, pte_t *, pmd_t *, unsigned int);
struct page *get_normal_page(struct vm_area_struct *vma, unsigned long addr, pte_t *pte);

#endif
