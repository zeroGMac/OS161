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
#include <thread.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <synch.h>


/* under dumbvm, always have 48k of user stack */
// #define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

void
vm_bootstrap(void)
{

	spinlock_init(&coremap_lock);
	/************ RB:Accomodate for last address misalignment ************/
	paddr_t fpaddr,lpaddr;
	ram_getsize(&fpaddr,&lpaddr);
	paddr_t availLast = lpaddr - (lpaddr%PAGE_SIZE);
	coremap_size = availLast/PAGE_SIZE;

	coremap = (struct coremap_entry *)PADDR_TO_KVADDR(fpaddr);
	KASSERT(coremap != NULL);
	paddr_t freeaddr_start = fpaddr + coremap_size*sizeof(struct coremap_entry);

	/************ RB:Accomodate for free address start misalignment ************/
	freeaddr_start = freeaddr_start + (PAGE_SIZE - (freeaddr_start%PAGE_SIZE));

	/************ RB:Mark fixed ************/
	unsigned int fixedIndex = freeaddr_start/PAGE_SIZE;
	search_start = fixedIndex;
	for (unsigned int i = 0; i <= availLast/PAGE_SIZE; ++i)
	{
		struct coremap_entry entry;
		if (i <=fixedIndex)
		{
			entry.p_state = PS_FIXED;
		}else{
			entry.p_state = PS_FREE;
		}
		entry.chunk_size = 1;
		entry.va = 0;
		entry.as = NULL;
		coremap[i] = entry;

	}
	vm_is_bootstrapped = true;
}

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);

	spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(int npages)
{

	if (vm_is_bootstrapped == true)
	{
		spinlock_acquire(&coremap_lock);
		for (unsigned int i = search_start; i < coremap_size; ++i)
		{
			if (coremap[i].p_state == PS_FREE)
			{
				bool allFree = true;
				for (unsigned int j = i+1; j < (unsigned int)npages && j<coremap_size; ++j)
				{
					if (coremap[j].p_state != PS_FREE)
					{
						allFree = false;
						break;
					}
				}
				if (allFree)
				{
					coremap[i].p_state = PS_FIXED;
					coremap[i].chunk_size = npages;
					paddr_t pa = i*PAGE_SIZE;
					spinlock_release(&coremap_lock);
					return PADDR_TO_KVADDR(pa);
				}
			}

		}
		spinlock_release(&coremap_lock);
		return 0;

	}else{
		paddr_t pa;
		pa = getppages(npages);
		if (pa==0) {
			return 0;
		}
		return PADDR_TO_KVADDR(pa);
	}

}

void
free_kpages(vaddr_t addr)
{
	if (vm_is_bootstrapped == true)
	{
		paddr_t pa = KVADDR_TO_PADDR(addr);
		int core_index = pa/PAGE_SIZE;
		spinlock_acquire(&coremap_lock);
		struct coremap_entry entry = coremap[core_index];
		int j = entry.chunk_size;
		for (int i = 0; i < j; ++i)
		{
			entry = coremap[core_index+i];
			entry.chunk_size = -1;
			entry.p_state = PS_FREE;
			coremap[core_index+i] = entry;
		}
		spinlock_release(&coremap_lock);
	}
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
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
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	as = curthread->t_addrspace;
	if (as == NULL) {
		/*
		 * No address space set up. This is probably a kernel
		 * fault early in boot. Return EFAULT so as to panic
		 * instead of getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	as_check_regions(as);



	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

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
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}

