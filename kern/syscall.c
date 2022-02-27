/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>

#include <kern/spinlock.h>

// Check page table entry perms
// PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
// but no other bits may be set.  See PTE_SYSCALL in inc/mmu.h.
#define CHECK_ARG_PERM(perm) \
do { \
	if (((perm) & ~PTE_SYSCALL) || !((perm) & PTE_P) || !((perm) & PTE_U)) {  \
		log("illegal perm: 0x%x", (perm)); \
		return -E_INVAL; \
	} \
} while (0)

#define CHECK_ARG_VA_ALIGNED(va) \
do { \
if ((uint32_t)(va) & 0Xfff) { \
	log("va: %p is not aligned.", (va)); \
	return -E_INVAL; \
} \
} while (0)

// Check virtual address, must be page aligned and under UTOP
#define CHECK_ARG_VA(va) \
do { \
	CHECK_ARG_VA_ALIGNED(va); \
	if ((va) >= (void*)UTOP) { \
		log("va: %p is above UTOP.", (va)); \
		return -E_INVAL; \
	} \
} while (0)

// get page_dir from envid
static pde_t*
envid2pgdir(envid_t envid)
{
	struct Env *e;
	int err;
	if ((err = envid2env(envid, &e, 1)) < 0) {
		log("bad envid: 0x%x", envid);
		return NULL;
	}
	return e->env_pgdir;
}

// Convert syscall description from number
static const char*
syscallname(int no)
{
#define RET_SYSCALL_NAME(no) \
case no: \
	return #no;
	switch (no)
	{
		RET_SYSCALL_NAME(SYS_cputs);
		RET_SYSCALL_NAME(SYS_cgetc);
		RET_SYSCALL_NAME(SYS_getenvid);
		RET_SYSCALL_NAME(SYS_env_destroy);
		RET_SYSCALL_NAME(SYS_page_alloc);
		RET_SYSCALL_NAME(SYS_page_map);
		RET_SYSCALL_NAME(SYS_page_unmap);
		RET_SYSCALL_NAME(SYS_exofork);
		RET_SYSCALL_NAME(SYS_env_set_status);
		RET_SYSCALL_NAME(SYS_env_set_pgfault_upcall);
		RET_SYSCALL_NAME(SYS_yield);
		RET_SYSCALL_NAME(SYS_ipc_try_send);
		RET_SYSCALL_NAME(SYS_ipc_recv);
		default:
			return "Unknown";
	}
}

// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static void
sys_cputs(const char *s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.
	user_mem_assert(curenv, s, len, 0);

	// Print the string supplied by the user.
	cprintf("%.*s", len, s);
}

// Read a character from the system console without blocking.
// Returns the character, or 0 if there is no input waiting.
static int
sys_cgetc(void)
{
	return cons_getc();
}

// Returns the current environment's envid.
static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	if (e == curenv)
		cprintf("[%08x] exiting gracefully\n", curenv->env_id);
	else
		cprintf("[%08x] destroying %08x\n", curenv->env_id, e->env_id);
	env_destroy(e);
	return 0;
}

// Deschedule current environment and pick a different one to run.
static void
sys_yield(void)
{
	sched_yield();
}

// Allocate a new environment.
// Returns envid of new environment, or < 0 on error.  Errors are:
//	-E_NO_FREE_ENV if no free environment is available.
//	-E_NO_MEM on memory exhaustion.
static envid_t
sys_exofork(void)
{
	// Create the new environment with env_alloc(), from kern/env.c.
	// It should be left as env_alloc created it, except that
	// status is set to ENV_NOT_RUNNABLE, and the register set is copied
	// from the current environment -- but tweaked so sys_exofork
	// will appear to return 0.
	struct Env* e;
	int err;
	if ((err = env_alloc(&e, curenv->env_id)) < 0) {
		log("env_alloc failed.");
		return err;
	}
	e->env_status = ENV_NOT_RUNNABLE;
	e->env_tf = curenv->env_tf;
	e->env_tf.tf_regs.reg_eax = 0;
	return e->env_id;
}

// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if status is not a valid status for an environment.
static int
sys_env_set_status(envid_t envid, int status)
{
	// Hint: Use the 'envid2env' function from kern/env.c to translate an
	// envid to a struct Env.
	// You should set envid2env's third argument to 1, which will
	// check whether the current environment has permission to set
	// envid's status.
	struct Env *e;
	int r;

	if ((r = envid2env(envid, &e, 1)) < 0) {
		log("bad envid, 0x%x", envid);
		return r;
	}
	if (status != ENV_RUNNABLE && status != ENV_NOT_RUNNABLE) {
		log("env 0x%x: bad param status: %d", envid, status);
		return -E_INVAL;
	}
	e->env_status = status;
	return 0;
}

// Set the page fault upcall for 'envid' by modifying the corresponding struct
// Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to
// 'func'.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
	struct Env *e;
	int r;

	if ((r = envid2env(envid, &e, 1)) < 0) {
		log("bad envid, 0x%x", envid);
		return r;
	}
	// check permison for page of func
	user_mem_assert(e, func, PGSIZE, PTE_U | PTE_P);
	e->env_pgfault_upcall = func;
	return 0;
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// The page's contents are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
//
// perm -- PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
//         but no other bits may be set.  See PTE_SYSCALL in inc/mmu.h.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	// Hint: This function is a wrapper around page_alloc() and
	//   page_insert() from kern/pmap.c.
	//   Most of the new code you write should be to check the
	//   parameters for correctness.
	//   If page_insert() fails, remember to free the page you
	//   allocated!

	struct Env *e;
	struct PageInfo *page;
	pde_t* pgdir;
	int err;
	// check va
	CHECK_ARG_VA(va);

	// check perm
	CHECK_ARG_PERM(perm);

	pgdir = envid2pgdir(envid);
	if (!pgdir) {
		return -E_BAD_ENV;
	}
	page = page_alloc(ALLOC_ZERO);
	if (!page) {
		log("No availble page.");
		return -E_NO_MEM;
	}
	if ((err = page_insert(pgdir, page, va, perm)) < 0) {
		page_free(page);
		log("No aviable page allocated for pte table");
		return err;
	}
	return 0;
}

// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Perm has the same restrictions as in sys_page_alloc, except
// that it also must not grant write access to a read-only
// page.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
//		or the caller doesn't have permission to change one of them.
//	-E_INVAL if srcva >= UTOP or srcva is not page-aligned,
//		or dstva >= UTOP or dstva is not page-aligned.
//	-E_INVAL is srcva is not mapped in srcenvid's address space.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in srcenvid's
//		address space.
//	-E_NO_MEM if there's no memory to allocate any necessary page tables.
static int
sys_page_map(envid_t srcenvid, void *srcva,
	     envid_t dstenvid, void *dstva, int perm)
{
	// Hint: This function is a wrapper around page_lookup() and
	//   page_insert() from kern/pmap.c.
	//   Again, most of the new code you write should be to check the
	//   parameters for correctness.
	//   Use the third argument to page_lookup() to
	//   check the current permissions on the page.

	pde_t *srcpgdir, *dstpgdir;
	pte_t *srcpte, *dstpte;
	struct PageInfo *srcpage;
	// check srcva and dstva
	CHECK_ARG_VA(srcva);
	CHECK_ARG_VA(dstva);
	// check perm
	CHECK_ARG_PERM(perm);

	if ((srcpgdir = envid2pgdir(srcenvid)) == NULL) {
		return -E_BAD_ENV;
	}
	if ((dstpgdir = envid2pgdir(dstenvid)) == NULL) {
		return -E_BAD_ENV;
	}
	srcpage = page_lookup(srcpgdir,srcva, &srcpte);
	if (!srcpage) {
		log("src va: %p doesn't mapped.", srcva);
		return -E_INVAL;
	}
	// check permision, must not map read-only page as writable 
	if (perm & PTE_W && !(*srcpte & PTE_W)) {
		log("dstva is read-only, but mapped as writable");
		return -E_INVAL;
	}
	if (page_insert(dstpgdir, srcpage, dstva, perm) < 0) {
		return -E_NO_MEM;
	}
	return 0;
}

// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
static int
sys_page_unmap(envid_t envid, void *va)
{
	// Hint: This function is a wrapper around page_remove().
	pde_t *pgdir;
	
	CHECK_ARG_VA(va);
	pgdir = envid2pgdir(envid);
	if (!pgdir) {
		log("env %0x has no pgdir.", envid);
		return -E_BAD_ENV;
	}
	page_remove(pgdir, va);
	return 0;
}

// Try to send 'value' to the target env 'envid'.
// If srcva < UTOP, then also send page currently mapped at 'srcva',
// so that receiver gets a duplicate mapping of the same page.
//
// The send fails with a return value of -E_IPC_NOT_RECV if the
// target is not blocked, waiting for an IPC.
//
// The send also can fail for the other reasons listed below.
//
// Otherwise, the send succeeds, and the target's ipc fields are
// updated as follows:
//    env_ipc_recving is set to 0 to block future sends;
//    env_ipc_from is set to the sending envid;
//    env_ipc_value is set to the 'value' parameter;
//    env_ipc_perm is set to 'perm' if a page was transferred, 0 otherwise.
// The target environment is marked runnable again, returning 0
// from the paused sys_ipc_recv system call.  (Hint: does the
// sys_ipc_recv function ever actually return?)
//
// If the sender wants to send a page but the receiver isn't asking for one,
// then no page mapping is transferred, but no error occurs.
// The ipc only happens when no errors occur.
//
// Returns 0 on success, < 0 on error.
// Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist.
//		(No need to check permissions.)
//	-E_IPC_NOT_RECV if envid is not currently blocked in sys_ipc_recv,
//		or another environment managed to send first.
//	-E_INVAL if srcva < UTOP but srcva is not page-aligned.
//	-E_INVAL if srcva < UTOP and perm is inappropriate
//		(see sys_page_alloc).
//	-E_INVAL if srcva < UTOP but srcva is not mapped in the caller's
//		address space.
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in the
//		current environment's address space.
//	-E_NO_MEM if there's not enough memory to map srcva in envid's
//		address space.
static int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
	struct Env *e;
	struct Env *cur;
	pte_t *pte;
	int r;

	if ((r = envid2env(envid, &e, 0)) < 0) {
		log("bad envid: %d, %e", envid, r);
		return r;
	}
	cur = curenv;
	if (!e->env_ipc_recving || e->env_status != ENV_NOT_RUNNABLE) {
		log("target env is not recving.");
		return -E_IPC_NOT_RECV;
	}
	// if srcva < UTOP, srcva must be page aligned
	// and check perm like above syscalls
	if (srcva < (void*)UTOP) {
		CHECK_ARG_VA_ALIGNED(srcva);
		CHECK_ARG_PERM(perm);
	}

	e->env_ipc_perm = 0;
	// check srcva is mapped
	if (srcva < (void*)UTOP && e->env_ipc_dstva < (void*)UTOP) {
		struct PageInfo *page;
		page = page_lookup(cur->env_pgdir, srcva, &pte);
		if (!page || !pte || !(*pte & (PTE_U | PTE_P))) {
			log("srcva is not mapped, va: %p.", srcva);
			return -E_INVAL;
		}
		// if srcva is read-only, perm must not be writable
		if ((perm & PTE_W) && !(*pte & PTE_W)) {
			log("srcva is read-only, but perm is writable, perm: 0x%x, pte: 0x%x.", perm, *pte);
			return -E_INVAL;
		}
		if ((r = page_insert(e->env_pgdir, page, e->env_ipc_dstva, perm)) < 0) {
			log("map page failed, srcva: %p, dstva: %p, perm: 0x%x", srcva, e->env_ipc_dstva, perm);
			return r;
		}
		e->env_ipc_perm = perm;
	}
	e->env_ipc_from = cur->env_id;
	e->env_ipc_value = value;
	e->env_ipc_recving = 0;
	e->env_status = ENV_RUNNABLE;
	e->env_tf.tf_regs.reg_eax = 0;
	return 0;
}

// Block until a value is ready.  Record that you want to receive
// using the env_ipc_recving and env_ipc_dstva fields of struct Env,
// mark yourself not runnable, and then give up the CPU.
//
// If 'dstva' is < UTOP, then you are willing to receive a page of data.
// 'dstva' is the virtual address at which the sent page should be mapped.
//
// This function only returns on error, but the system call will eventually
// return 0 on success.
// Return < 0 on error.  Errors are:
//	-E_INVAL if dstva < UTOP but dstva is not page-aligned.
static int
sys_ipc_recv(void *dstva)
{
	struct Env *cur;
	int r;
	cur = curenv;
	if (dstva < (void*)UTOP) {
		CHECK_ARG_VA_ALIGNED(dstva);
	}
	cur->env_status = ENV_NOT_RUNNABLE;
	cur->env_ipc_recving = 1;
	cur->env_ipc_dstva = dstva;
	sched_yield();
	return 0;
}

// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	log("syscall %s", syscallname(syscallno));
	// Call the function corresponding to the 'syscallno' parameter.
	// Return any appropriate return value.
	switch (syscallno) {
	case SYS_cputs:
		sys_cputs((const char*)a1, (size_t)a2);
		return 0;
	case SYS_cgetc:
		return sys_cgetc();
	case SYS_getenvid:
		return sys_getenvid();
	case SYS_env_destroy:
		return sys_env_destroy((envid_t)a1);
	case SYS_yield:
		sys_yield();
		return 0;
	case SYS_exofork:
		return sys_exofork();
	case SYS_env_set_status:
		return sys_env_set_status((envid_t)a1, a2);
	case SYS_page_alloc:
		return sys_page_alloc((envid_t)a1, (void*)a2, a3);
	case SYS_page_map:
		return sys_page_map((envid_t)a1, (void*)a2, (envid_t)a3, (void*)a4, a5);
	case SYS_page_unmap:
		return sys_page_unmap((envid_t)a1, (void*)a2);
	case SYS_env_set_pgfault_upcall:
		return sys_env_set_pgfault_upcall((envid_t)a1, (void*)a2);
	case SYS_ipc_try_send:
		return sys_ipc_try_send((envid_t)a1, a2, (void*)a3, a4);
	case SYS_ipc_recv:
		return sys_ipc_recv((void*)a1);
	default:
		return -E_INVAL;
	}
}

uint32_t
sysenter_wrapper(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t eip, uint32_t esp)
{
#define STORE_TF \
curenv->env_tf.tf_cs = GD_UT | 3; \
curenv->env_tf.tf_eip = eip; \
curenv->env_tf.tf_ss = GD_UD | 3; \
curenv->env_tf.tf_esp = esp; \
curenv->env_tf.tf_eflags = read_eflags() | FL_IF;
	log("sysenter %s, eip: %p, esp: %p", syscallname(syscallno), eip, esp);
	int r;
	asm volatile("cld" ::: "cc");
	lock_kernel();
	assert(curenv);
	switch(syscallno) {
	case SYS_cputs:
		sys_cputs((const char*)a1, (size_t)a2);
		r = 0;
		break;
	case SYS_cgetc:
		r = sys_cgetc();
		break;
	case SYS_getenvid:
		r = sys_getenvid();
		break;
	case SYS_env_destroy:
		r = sys_env_destroy((envid_t)a1);
		break;
	case SYS_exofork:
		r = sys_exofork();
		break;
	case SYS_env_set_status:
		r = sys_env_set_status((envid_t)a1, a2);
		break;
	case SYS_page_alloc:
		r = sys_page_alloc((envid_t)a1, (void*)a2, a3);
		break;
	case SYS_page_unmap:
		r = sys_page_unmap((envid_t)a1, (void*)a2);
		break;
	case SYS_env_set_pgfault_upcall:
		r = sys_env_set_pgfault_upcall((envid_t)a1, (void*)a2);
		break;
	case SYS_ipc_try_send:
		r = sys_ipc_try_send((envid_t)a1, a2, (void*)a3, a4);
		break;
	case SYS_yield:
		STORE_TF;
		sys_yield();
		return 0;
	case SYS_ipc_recv:
		STORE_TF;
		r = sys_ipc_recv((void*)a1);
		break;
	default:
		r = -E_INVAL;
	}

	unlock_kernel();
	return r;
}

