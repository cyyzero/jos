// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

extern volatile pte_t uvpt[];     // VA of "virtual page table"
extern volatile pde_t uvpd[];     // VA of current page directory
//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	uintptr_t addr = utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).
	pde_t pde = uvpd[PDX(addr)];
	pte_t pte = uvpt[addr >> PGSHIFT];
	if (!((pde & (PTE_P | PTE_U | PTE_W)) &&
		   pte & (PTE_P | PTE_U | PTE_COW))) {
		panic("Not COW page, pde: 0x%x, pte: 0x%x.\n", pde, pte);
	}

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.
	addr = ROUNDDOWN(addr, PGSIZE);

	r = sys_page_alloc(0, PFTEMP, PTE_P | PTE_U | PTE_W);
	if (r < 0) {
		cprintf("sys_page_alloc failed, %e.\n", r);
		return;
	}

	memmove(PFTEMP, (void*)addr, PGSIZE);
	r = sys_page_map(0, PFTEMP, 0, (void*)addr, PTE_P | PTE_U | PTE_W);
	if (r < 0) {
		cprintf("sys_page_map failed, %e.\n", r);
		sys_page_unmap(0, PFTEMP);
		return;
	}
	r = sys_page_unmap(0, PFTEMP);
	if (r < 0) {
		cprintf("sys_page_unmap PFTEMP failed, %e.\n", r);
		return;
	}
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn, int is_cow)
{
	int r;
	void* va = (void*)(pn << PGSHIFT);
	int perm = PTE_U | PTE_P;
	// if read-only, map to the same page
	// if writable, map to a new page which is marked as PTE_COW
	if (is_cow) {
		perm |= PTE_COW;
	}
	r = sys_page_map(0, va, envid, va, perm);
	if (r < 0) {
		cprintf("sys_page_map from parent to child failed: %e.\n", r);
		return r;
	}

	if (is_cow) {
		r = sys_page_map(0, va, 0, va, perm);
		if (r < 0) {
			cprintf("sys_page_map remap child failed: %e.\n", r);
			return r;
		}
	}
	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	extern unsigned char end[];
	int r;
	envid_t eid;
	set_pgfault_handler(pgfault);
	eid = sys_exofork();
	if (eid < 0) {
		cprintf("fork failed, %e", eid);
		return eid;
	}
	// child
	if (eid == 0) {
		thisenv = &envs[ENVX(sys_getenvid())];
		return eid;
	}

	// parent
	// iterate each pde, map writable pages
	for (int pdeno = 0; pdeno <= PDX(UTOP-1); ++pdeno) {
		// if present, iterate each pte
		if (uvpd[pdeno] & (PTE_P | PTE_W | PTE_U)) {
			int entry_num = NPTENTRIES;
			if (pdeno == PDX(UTOP-1)) {
				entry_num = PTX(UTOP-1) + 1;
			}
			for (int pteno = 0; pteno < entry_num; ++pteno) {
				int pn = pdeno * NPDENTRIES + pteno;
				pte_t pte = uvpt[pn];
				if (pte & (PTE_P | PTE_U)) {
					if (pte & PTE_W || pte & PTE_COW) {
						// user exception stack
						if (pn == ((UXSTACKTOP - PGSIZE) >> PGSHIFT)) {
							continue; 
						}
						if ((r = duppage(eid, pn, 1)) < 0) {
							return r;
						}
					} else {
						if ((r = duppage(eid, pn, 0)) < 0) {
							return r;
						}
					}
				}
			}
		}
	}

	// map user exception stack for child
	r = sys_page_alloc(eid, (void*)(UXSTACKTOP -PGSIZE), PTE_P | PTE_U | PTE_W);
	if (r < 0) {
		cprintf("map user exception stack failed, %e.\n", r);
		return r;
	}
	extern void _pgfault_upcall(void);
	// set the pgfault_handler for child
	r = sys_env_set_pgfault_upcall(eid, _pgfault_upcall);
	if (r < 0) {
		cprintf("env set pgfault upcall failed, %e.\n", r);
		return r;
	}
	r = sys_env_set_status(eid, ENV_RUNNABLE);
	if (r < 0) {
		cprintf("set child status failed, %e.\n", r);
		return r;
	}
	return eid;
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
