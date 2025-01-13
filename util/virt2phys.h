#ifndef __VIRT2PHYS_H__
#define __VIRT2PHYS_H__

#ifdef __cplusplus
extern "C" {
#endif
	
	int virt_to_phys(uint64_t *paddr, uint64_t vaddr);
	void read_maps(pid_t pid);
	void print_pagemap_entry(pid_t pid, unsigned long vaddr_start, unsigned long vaddr_end);

#ifdef __cplusplus
}
#endif



#endif