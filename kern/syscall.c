/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/fs.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>

#include <kern/spinlock.h>

#define debug 0
#define FSIPCBUF2USTACK(addr)	((void*) (addr)  - (void*)fsipcbuf + (USTACKTOP - PGSIZE))

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

static pde_t*
envid2pgdirWithCheck(envid_t envid, bool perm)
{
	struct Env *e;
	int err;
	if ((err = envid2env(envid, &e, perm)) < 0) {
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
	int check = 1;

	if (curenv->env_type ==ENV_TYPE_FS)
		check = 0;

	if ((r = envid2env(envid, &e, check)) < 0)
		return r;
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
	struct Env *e, *cur;
	int r;
	int check = 1;
	cur = curenv;
	if (cur->env_type == ENV_TYPE_FS && cur->env_id != envid) {
		check = 0;
	}
	if ((r = envid2env(envid, &e, check)) < 0) {
		log("bad envid, 0x%x", envid);
		return r;
	}
	if (status != ENV_RUNNABLE && status != ENV_NOT_RUNNABLE) {
		log("env 0x%x: bad param status: %d", envid, status);
		return -E_INVAL;
	}
	e->env_status = status;
	Debug("eip is %p", e->env_tf.tf_eip);
	return 0;
}

// Set envid's trap frame to 'tf'.
// tf is modified to make sure that user environments always run at code
// protection level 3 (CPL 3), interrupts enabled, and IOPL of 0.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
	// LAB 5: Your code here.
	// Remember to check whether the user has supplied us with a good
	// address!
	int r;
	struct Env *env;
	int check = 1;
	if (curenv->env_type == ENV_TYPE_FS) {
		check = 0;
	}
	if ((r = envid2env(envid, &env, check)) < 0) {
		return r;
	}
	if ((uintptr_t)tf >= UTOP) {
		return -E_BAD_ENV;
	}
	// run at user mode
	tf->tf_cs |= 3;
	tf->tf_es |= 3;
	tf->tf_ds |= 3;
	// enable interrupt
	tf->tf_eflags |= FL_IF;
	// set IOPL to 0
	tf->tf_eflags &= ~FL_IOPL_MASK;
	env->env_tf = *tf;
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

	struct Env *e, *cur;
	struct PageInfo *page;
	pde_t* pgdir;
	int err;
	int check = 1;
	// Debug("env 0x%x asked to alloc page for env 0x%x at va %p", curenv->env_id, envid, va);
	// check va
	CHECK_ARG_VA(va);

	// check perm
	CHECK_ARG_PERM(perm);

	cur = curenv;
	// allow FS to allocate pages
	if (cur->env_type == ENV_TYPE_FS && envid != cur->env_id) {
		check = 0;
	}
	pgdir = envid2pgdirWithCheck(envid, check);
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
		Debug("No aviable page allocated for pte table");
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
	struct Env *env;
	struct PageInfo *srcpage;
	int r;
	bool checkDstEnv = 1;

	// check srcva and dstva
	CHECK_ARG_VA(srcva);
	CHECK_ARG_VA(dstva);
	// check perm
	CHECK_ARG_PERM(perm);

	if ((r = envid2env(srcenvid, &env, 1)) < 0) {
		log("Envid invalid, eid: 0x%x, err: %e", srcenvid, r);
		return r;
	}
	srcpgdir = env->env_pgdir;
	// allow FS map pages to any env
	if (env->env_type == ENV_TYPE_FS) {
		checkDstEnv = 0;
	}

	if ((dstpgdir = envid2pgdirWithCheck(dstenvid, checkDstEnv)) == NULL) {
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
	int check = 1;
	
	if (curenv->env_type == ENV_TYPE_FS) {
		check = 0;
	}
	CHECK_ARG_VA(va);
	pgdir = envid2pgdirWithCheck(envid, check);
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
		Debug("bad envid: %x, %e", envid, r);
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
	static int visited;
	if (!visited) {
		// enable other user program running
		if (envs[1].env_status == ENV_BEFORE_FS)
			envs[1].env_status = ENV_RUNNABLE;
		visited = 1;
	}
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

// Receive a value via IPC and return it.
// If 'pg' is nonnull, then any page sent by the sender will be mapped at
//	that address.
// If 'from_env_store' is nonnull, then store the IPC sender's envid in
//	*from_env_store.
// If 'perm_store' is nonnull, then store the IPC sender's page permission
//	in *perm_store (this is nonzero iff a page was successfully
//	transferred to 'pg').
// If the system call fails, then store 0 in *fromenv and *perm (if
//	they're nonnull) and return the error.
// Otherwise, return the value sent by the sender
//
// Hint:
//   Use 'thisenv' to discover the value and who sent it.
//   If 'pg' is null, pass sys_ipc_recv a value that it will understand
//   as meaning "no page".  (Zero is not the right value, since that's
//   a perfectly valid place to map a page.)
static int32_t
ipc_recv(envid_t *from_env_store, void *pg, int *perm_store)
{
	int r;
	struct Env *thisenv = curenv;
	if (pg == NULL) {
		pg = (void*)UTOP;
	}
	r = sys_ipc_recv(pg);
	if (r < 0) {
		cprintf("sys_ipc_recv failed, %e\n", r);
		*from_env_store = 0;
		*perm_store = 0;
		return r;
	}
	if (from_env_store) {
		*from_env_store = thisenv->env_ipc_from;
	}
	if (perm_store) {
		*perm_store = thisenv->env_ipc_perm;
	}
	return thisenv->env_ipc_value;
}

// Send 'val' (and 'pg' with 'perm', if 'pg' is nonnull) to 'toenv'.
// This function keeps trying until it succeeds.
// It should panic() on any error other than -E_IPC_NOT_RECV.
//
// Hint:
//   If 'pg' is null, pass sys_ipc_try_send a value that it will understand
//   as meaning "no page".  (Zero is not the right value.)
static int
ipc_send(envid_t to_env, uint32_t val, void *pg, int perm)
{
	int r;
	if (pg == NULL) {
		pg = (void*)UTOP;
	}
	if (debug)
		Debug("send pg: %p, perm: %x\n", pg, perm);
	while (true) {
		r = sys_ipc_try_send(to_env, val, pg, perm);
		if (r == 0) {
			break;
		} else if (r == -E_IPC_NOT_RECV) {
			Debug("Fs is not recving.");
			curenv->env_tf.tf_eip -= 2;
			sys_page_unmap(0, UTEMP);
			sys_yield();
		} else {
			log("send failed, eid: 0x%x, pg: %d, perm: 0x%x, err: %e \n", to_env, pg, perm, r);
			return -E_FAULT;
		}
	}
	return 0;
}

// Find the first environment of the given type.  We'll use this to
// find special environments.
// Returns 0 if no such environment exists.
static envid_t
ipc_find_env(enum EnvType type)
{
	int i;
	for (i = 0; i < NENV; i++)
		if (envs[i].env_type == type)
			return envs[i].env_id;
	return 0;
}

static int
fsipc(unsigned type, void *fsipcbuf)
{
	static envid_t fsenv;
	if (fsenv == 0)
		fsenv = ipc_find_env(ENV_TYPE_FS);

	int r;

	// assert(sizeof(fsipcbuf) == PGSIZE);
	if (debug)
		Debug("fsipcbuf: %p\n", fsipcbuf);
	r = ipc_send(fsenv, type, fsipcbuf, PTE_P | PTE_W | PTE_U);
	if (r < 0) {
		if (debug)
			Debug("ipc send failed, %e", r);
	}
	return 0;
}

// Set up the initial stack page for the new child process with envid 'child'
// using the arguments array pointed to by 'argv',
// which is a null-terminated array of pointers to null-terminated strings.
//
// On success, returns 0 and sets *init_esp
// to the initial stack pointer with which the child should start.
// Returns < 0 on failure.
static int
init_stack_and_free_user_vm(struct Env *e, const char **argv, void *fsipcbuf)
{
	cprintf("enter init stack\n");
	size_t string_size;
	int argc, i, r;
	char *string_store;
	uintptr_t *argv_store;

	// Count the number of arguments (argc)
	// and the total amount of space needed for strings (string_size).
	string_size = 0;
	for (argc = 0; argv[argc] != 0; argc++)
		string_size += strlen(argv[argc]) + 1;
	Debug("argc is %d, string size is %d\n", argc, string_size);
	// Determine where to place the strings and the argv array.
	// Set up pointers into fsipcbuf; we'll map a page
	// there later, then remap that page into the child environment
	// at (USTACKTOP - PGSIZE).
	// strings is the topmost thing on the stack.
	string_store = fsipcbuf + PGSIZE - string_size;
	// argv is below that.  There's one argument pointer per argument, plus
	// a null pointer.
	argv_store = (uintptr_t*) (ROUNDDOWN(string_store, 4) - 4 * (argc + 1));

	Debug("string_store: %p argv_store %p\n", string_store, argv_store);

	// Make sure that argv, strings, and the 2 words that hold 'argc'
	// and 'argv' themselves will all fit in a single stack page.
	if ((void*) (argv_store - 2) < fsipcbuf)
		return -E_NO_MEM;


	//	* Initialize 'argv_store[i]' to point to argument string i,
	//	  for all 0 <= i < argc.
	//	  Also, copy the argument strings from 'argv' into the
	//	  newly-allocated stack page.
	//
	//	* Set 'argv_store[argc]' to 0 to null-terminate the args array.
	//
	//	* Push two more words onto the child's stack below 'args',
	//	  containing the argc and argv parameters to be passed
	//	  to the child's umain() function.
	//	  argv should be below argc on the stack.
	//	  (Again, argv should use an address valid in the child's
	//	  environment.)
	//
	//	* Set *init_esp to the initial stack pointer for the child,
	//	  (Again, use an address valid in the child's environment.)
	for (i = 0; i < argc; i++) {
		argv_store[i] = FSIPCBUF2USTACK(string_store);
		strcpy(string_store, argv[i]);
		Debug("%dth argv %s", i, string_store);
		string_store += strlen(argv[i]) + 1;
	}
	argv_store[argc] = 0;
	Debug("string_store %p", string_store);
	assert(string_store == fsipcbuf + PGSIZE);

	argv_store[-1] = FSIPCBUF2USTACK(argv_store);
	argv_store[-2] = argc;

	e->env_tf.tf_esp = FSIPCBUF2USTACK(&argv_store[-2]);
	Debug("env esp is %p", e->env_tf.tf_esp);

	// free all previous user vm
	// env_free_user_vm(e);
	// After completing the stack, map it into the child's address space
	// and unmap it from ours!
	if ((r = sys_page_alloc(0, (void*) (USTACKTOP - PGSIZE), PTE_P | PTE_U | PTE_W)) < 0) {
		return r;
	}
	memmove((void*) (USTACKTOP - PGSIZE), fsipcbuf, PGSIZE);
	memset(fsipcbuf, 0, PGSIZE);
	return 0;
}

// exec an ELF, replace the .text and data segments
static int
sys_exec(const char* pathname, const char*argv[])
{
	int r, len;
	struct Env *e;
	union Fsipc *fsipcbuf;
	if ((r = sys_page_alloc(0, UTEMP, PTE_P | PTE_U | PTE_W)) < 0) {
		goto error;
	}
	
	if (debug)
		Debug("sys_pag_alloc %e", r);
	char pathbuf[MAXPATHLEN];
	fsipcbuf = (union Fsipc *)UTEMP;

	e = curenv;
	e->env_status = ENV_NOT_RUNNABLE;

	// copy pathname
	strncpy(pathbuf, pathname, MAXPATHLEN);
	pathbuf[MAXPATHLEN-1] = 0;
	// init stack
	if ((r = init_stack_and_free_user_vm(e, argv, (void*)fsipcbuf)) < 0) {
		Debug("init stack failed, %e\n", r);
		goto error;
	}
	if (debug)
		Debug("Finish stack init\n");
	// copy pathname to fsipcbuf
	strncpy(fsipcbuf->load.req_path, pathbuf, MAXPATHLEN);
	Debug("fsipcbuf.load.req_path: %p %s %s\n", &fsipcbuf->load.req_path, fsipcbuf->load.req_path, (void*)fsipcbuf);
	// load elf
	if ((r = fsipc(FSREQ_LOAD, fsipcbuf)) < 0) {
		Debug("FS ipc load failed", r);
		goto error;
	}
	if (debug)
		Debug("finish exec, env status %d", e->env_status);
	return 0;
error:
	sys_page_unmap(0, UTEMP);
	return r;
}

// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	// log("syscall %s", syscallname(syscallno));
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
	case SYS_env_set_trapframe:
		return sys_env_set_trapframe((envid_t)a1, (struct Trapframe*)a2);
	case SYS_exec:
		return sys_exec((const char*)a1, (const char**)a2);
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
	case SYS_env_set_trapframe:
		STORE_TF;
		r = sys_env_set_trapframe((envid_t)a1, (struct Trapframe *)a2);
		break;
	case SYS_exec:
		STORE_TF;
		r = sys_exec((const char*)a1, (const char**)a2);
	default:
		r = -E_INVAL;
	}

	unlock_kernel();
	return r;
}

