/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include "opt-A3.h"

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

#if OPT_A3

// spinlock for core map
static struct spinlock coremapLock = SPINLOCK_INITIALIZER;

// coreMap definition 
typedef struct CoreMap {
	unsigned long size;
	unsigned long* entries;
	vaddr_t start;
	bool loadComplete;
} CoreMap; 

static CoreMap cm = {0, NULL, 0, false};

// ith page's paddr = cm.start + i * PAGE_SIZE;
#endif

void vm_bootstrap(void) {
	#if OPT_A3
	paddr_t lo;
	paddr_t hi;
	ram_getsize(&lo, &hi);

	// where the entriy starts
	cm.entries = (unsigned long*) PADDR_TO_KVADDR(lo);

	// divisible by PAGE_SIZE
	unsigned long tempPageNum = (hi - lo) / PAGE_SIZE;
	lo = ROUNDUP(lo + tempPageNum * sizeof(unsigned long), PAGE_SIZE);

	// initialization 
	cm.size = (hi - lo) / PAGE_SIZE;
	cm.start = PADDR_TO_KVADDR(lo);
	for (unsigned long i = 0; i < cm.size; i++) {
		cm.entries[i] = 0;
	}
	// finish loading
	cm.loadComplete = true;
	#endif	
}

/*static paddr_t getppages(unsigned long npages) {
	paddr_t addr;
	
	spinlock_acquire(&stealmem_lock);
	addr = ram_stealmem(npages);
	spinlock_release(&stealmem_lock);

	return addr;
}*/
#if OPT_A3
static vaddr_t getppages(int npages) {   // easier approach of getppages
	vaddr_t vAddr = 0;
	spinlock_acquire(&coremapLock);
	for (unsigned long i = 0; i < cm.size; i++) {
		if (cm.entries[i] == 0) {
			int countAvailablePage = 0;
			for (unsigned long j = i; j < cm.size; j++) {
				if (cm.entries[j] == 0) {
					countAvailablePage += 1;
					if (countAvailablePage == npages) { 
						break; 
					}
				} else {
					break;
				}
			}
			if (countAvailablePage == npages) { // found space, start marking used 
				//kprintf("reached here with number: %d\n", countAvailablePage);
				unsigned long pos = i;
				for (int k = 1; k <= npages; k++) {
					cm.entries[pos] = k;
					pos += 1;
				}
				vAddr = cm.start + i * PAGE_SIZE;
				spinlock_release(&coremapLock);
				return vAddr;
			}
		}
	}
	spinlock_release(&coremapLock);
	return 0;
}

#endif
/* Allocate/free some kernel-space virtual pages */
vaddr_t alloc_kpages(int npages) { 
	paddr_t pa;
	#if OPT_A3
		if (cm.loadComplete == false) {
			spinlock_acquire(&stealmem_lock);
			pa = ram_stealmem(npages);
			spinlock_release(&stealmem_lock);
		} else {
			return getppages(npages);
		}
	#else
		spinlock_acquire(&stealmem_lock);
		pa = ram_stealmem(npages);
		spinlock_release(&stealmem_lock);
	#endif
	
	return PADDR_TO_KVADDR(pa);
}

void free_kpages(vaddr_t addr) {
	/* nothing - leak the memory. */
	#if OPT_A3
		spinlock_acquire(&coremapLock);
		unsigned long pos = (addr - cm.start) / PAGE_SIZE;
		unsigned long startValue = cm.entries[pos];
		while (true) {
			unsigned long currValue = cm.entries[pos];
			if (currValue != startValue) {
				break;
			} else {
				cm.entries[pos] = 0; // becomes avaiable 
				pos += 1;
				startValue += 1;
			}
		}
		spinlock_release(&coremapLock);
	#else
		(void)addr;
	#endif 
}

void vm_tlbshootdown_all(void) {
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void vm_tlbshootdown(const struct tlbshootdown *ts) {
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int vm_fault(int faulttype, vaddr_t faultaddress) {
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
			#if OPT_A3
				return EROFS; // read-only file system 

		/* We always create pages read-write, so we can't get this */
			#else 
				panic("dumbvm: got VM_FAULT_READONLY\n");
			#endif
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	#if OPT_A3
		KASSERT(as->as_vbase1 != 0);
		KASSERT(as->as_npages1 != 0);
		KASSERT(as->as_vbase2 != 0);
		KASSERT(as->as_npages2 != 0);
		KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
		KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	#else
		KASSERT(as->as_vbase1 != 0);
		KASSERT(as->as_pbase1 != 0);
		KASSERT(as->as_npages1 != 0);
		KASSERT(as->as_vbase2 != 0);
		KASSERT(as->as_pbase2 != 0);
		KASSERT(as->as_npages2 != 0);
		KASSERT(as->as_stackpbase != 0);
		KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
		KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
		KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
		KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
		KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
	#endif 
	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;
	
	#if OPT_A3
		bool codeSegment = false;
		int pos = 0;
		if (faultaddress >= vbase1 && faultaddress < vtop1) {
			codeSegment = true;
			pos = (int)((faultaddress - vbase1) / PAGE_SIZE);
			paddr = as->as_pbase1[pos];
		}
		else if (faultaddress >= vbase2 && faultaddress < vtop2) {
			pos = (int)((faultaddress - vbase2) / PAGE_SIZE);
			paddr = as->as_pbase2[pos];
		}
		else if (faultaddress >= stackbase && faultaddress < stacktop) {
			pos = (int)((faultaddress - stackbase) / PAGE_SIZE);
			paddr = as->as_stackpbase[pos];
		}
		else {
			return EFAULT;
		} 
	#else 
		if (faultaddress >= vbase1 && faultaddress < vtop1) {
			paddr = (faultaddress - vbase1) + as->as_pbase1;
		}
		else if (faultaddress >= vbase2 && faultaddress < vtop2) {
			paddr = (faultaddress - vbase2) + as->as_pbase2;
		}
		else if (faultaddress >= stackbase && faultaddress < stacktop) {
			paddr = (faultaddress - stackbase) + as->as_stackpbase;
		}
		else {
			return EFAULT;
		} 
	#endif
	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		#if OPT_A3
			if (codeSegment == true && as->loadelfComplete == true) {
				elo &= ~TLBLO_DIRTY;
			}
		#endif
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}
	#if OPT_A3
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		if (codeSegment == true && as->loadelfComplete == true) {
			elo &= ~TLBLO_DIRTY;
		}
		tlb_random(ehi, elo);
		splx(spl);
		return 0;
	#else
		kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
		splx(spl);
		return EFAULT;
	#endif
}

struct addrspace* as_create(void) {
	//kprintf("called as_create\n");
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}
	#if OPT_A3
		as->as_vbase1 = 0;
		as->as_pbase1 = NULL;
		as->as_npages1 = 0;
		as->as_vbase2 = 0;
		as->as_pbase2 = NULL;
		as->as_npages2 = 0;
		as->as_stackpbase = NULL;
		as->loadelfComplete = false;
	#else
		as->as_vbase1 = 0;
		as->as_pbase1 = 0;
		as->as_npages1 = 0;
		as->as_vbase2 = 0;
		as->as_pbase2 = 0;
		as->as_npages2 = 0;
		as->as_stackpbase = 0;
	#endif
	//kprintf("created as\n");
	return as;
}

void as_destroy(struct addrspace *as) {
	//kprintf("called as_destroy\n");
	#if OPT_A3
		unsigned int length = as->as_npages1 >= as->as_npages2 ? as->as_npages1 : as->as_npages2; 
		length = length >= DUMBVM_STACKPAGES ? length : DUMBVM_STACKPAGES;
		for (unsigned int i = 0; i < length; i++) {
			if (i < as->as_npages1) {
				free_kpages(PADDR_TO_KVADDR(as->as_pbase1[i]));
			}
			if (i < as->as_npages2) {
				free_kpages(PADDR_TO_KVADDR(as->as_pbase2[i]));
			}
			if (i < DUMBVM_STACKPAGES) {
				free_kpages(PADDR_TO_KVADDR(as->as_stackpbase[i]));
			}
		}
		free_kpages((vaddr_t)as->as_pbase1);
		free_kpages((vaddr_t)as->as_pbase2);
		free_kpages((vaddr_t)as->as_stackpbase);
	#else
		kfree(as);
	#endif
}

void as_activate(void) {
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void as_deactivate(void) {
	/* nothing */
}

int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 			 int readable, int writeable, int executable) {
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	#if OPT_A3
		as->readPermission = readable;
		as->writePermission = writeable;
		as->executePermission = executable;
	#else
	/* We don't use these - all pages are read-write */
		(void)readable;
		(void)writeable;
		(void)executable;
	#endif

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static void as_zero_region(paddr_t paddr, unsigned npages) {
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int as_prepare_load(struct addrspace *as) {
	#if OPT_A3
		//kprintf("called as_prepare_load\n");
		KASSERT(as->as_pbase1 == NULL);
		KASSERT(as->as_pbase2 == NULL);
		KASSERT(as->as_stackpbase == NULL);

		unsigned long space = ROUNDUP(as->as_npages1 * sizeof(paddr_t), PAGE_SIZE);
		unsigned int numPages = space / PAGE_SIZE;
		//kprintf("break point 1\n");
		vaddr_t result = alloc_kpages(numPages);
		if (result == 0) { return ENOMEM; } 
		//kprintf("break point 4\n");
		as->as_pbase1 = (paddr_t*) result;
		for (size_t i = 0; i < as->as_npages1; i++) {
			result = alloc_kpages(1);
			//kprintf("break point 5\n");
			//kprintf("%d\n", result);
			if (result == 0) { 
				return ENOMEM; 
			}
			//kprintf("break point 6\n");
			as_zero_region(KVADDR_TO_PADDR(result), 1);
			as->as_pbase1[i] = KVADDR_TO_PADDR(result);
			//kprintf("break point 7\n");
		}
		//kprintf("break point 2\n");
		space = ROUNDUP(as->as_npages2 * sizeof(paddr_t), PAGE_SIZE);
		numPages = space / PAGE_SIZE;

		result = alloc_kpages(numPages);
		if (result == 0) { return ENOMEM; } 
		as->as_pbase2 = (paddr_t*) result;
		for (size_t i = 0; i < as->as_npages2; i++) {
			result = alloc_kpages(1);
			if (result == 0) { return ENOMEM; }
			as_zero_region(KVADDR_TO_PADDR(result), 1);
			as->as_pbase2[i] = KVADDR_TO_PADDR(result);
		}
		//kprintf("break point 3\n");
		space = ROUNDUP(DUMBVM_STACKPAGES * sizeof(paddr_t), PAGE_SIZE);
		numPages = space / PAGE_SIZE;

		result = alloc_kpages(numPages);
		if (result == 0) { return ENOMEM; } 
		as->as_stackpbase = (paddr_t*) result;
		for (size_t i = 0; i < DUMBVM_STACKPAGES; i++) {
			result = alloc_kpages(1);
			if (result == 0) { return ENOMEM; }
			as_zero_region(KVADDR_TO_PADDR(result), 1);
			as->as_stackpbase[i] = KVADDR_TO_PADDR(result);
		}
		//kprintf("finish as_prepare_load\n");
		KASSERT(as->as_pbase1 != NULL);
		KASSERT(as->as_pbase2 != NULL);
		KASSERT(as->as_stackpbase != NULL);
	#else
		KASSERT(as->as_pbase1 == 0);
		KASSERT(as->as_pbase2 == 0);
		KASSERT(as->as_stackpbase == 0);
		
		as->as_pbase1 = getppages(as->as_npages1);
		if (as->as_pbase1 == 0) {
			return ENOMEM;
		}

		as->as_pbase2 = getppages(as->as_npages2);
		if (as->as_pbase2 == 0) {
			return ENOMEM;
		}

		as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
		if (as->as_stackpbase == 0) {
			return ENOMEM;
		}	
	
		as_zero_region(as->as_pbase1, as->as_npages1);
		as_zero_region(as->as_pbase2, as->as_npages2);
		as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);
	#endif
	return 0;
}

int as_complete_load(struct addrspace *as) {
	#if OPT_A3
		as->loadelfComplete = true;
		as_activate();
	#else
		(void)as;
	#endif

	return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr) {
	#if OPT_A3
		KASSERT(as->as_stackpbase != NULL);
	#else
		KASSERT(as->as_stackpbase != 0);
	#endif

	*stackptr = USERSTACK;
	return 0;
}

int as_copy(struct addrspace *old, struct addrspace **ret) {
	struct addrspace *new;
	//kprintf("called as_copy\n");
	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

	#if OPT_A3  // loading status and permissions
		new->loadelfComplete = old->loadelfComplete;
		new->readPermission = old->readPermission;
		new->writePermission = old->writePermission;
		new->executePermission = old->executePermission;
	#endif

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	#if OPT_A3
		KASSERT(new->as_pbase1 != NULL);
		KASSERT(new->as_pbase2 != NULL);
		KASSERT(new->as_stackpbase != NULL);

		unsigned int length = old->as_npages1 >= old->as_npages2 ? old->as_npages1 : old->as_npages2; 
		length = length >= DUMBVM_STACKPAGES ? length : DUMBVM_STACKPAGES;
		for (unsigned int i = 0; i < length; i++) {
			if (i < old->as_npages1) {
				memmove((void *)PADDR_TO_KVADDR(new->as_pbase1[i]),
      				(const void *)PADDR_TO_KVADDR(old->as_pbase1[i]), PAGE_SIZE);
			}
			if (i < old->as_npages2) {
				memmove((void *)PADDR_TO_KVADDR(new->as_pbase2[i]),
      				(const void *)PADDR_TO_KVADDR(old->as_pbase2[i]), PAGE_SIZE);
			}
			if (i < DUMBVM_STACKPAGES) {
				memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase[i]),
    				(const void *)PADDR_TO_KVADDR(old->as_stackpbase[i]), PAGE_SIZE);
			}
		}
	#else
		KASSERT(new->as_pbase1 != 0);
		KASSERT(new->as_pbase2 != 0);
		KASSERT(new->as_stackpbase != 0);

		memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
			(const void *)PADDR_TO_KVADDR(old->as_pbase1),
			old->as_npages1*PAGE_SIZE);

		memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
			(const void *)PADDR_TO_KVADDR(old->as_pbase2),
			old->as_npages2*PAGE_SIZE);

		memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
			(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
			DUMBVM_STACKPAGES*PAGE_SIZE);
	#endif

	*ret = new;
	return 0;
}
