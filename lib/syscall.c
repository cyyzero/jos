// System call stubs.

#include <inc/syscall.h>
#include <inc/lib.h>

// #define USE_SYSENTER

static inline int32_t
sysenter(int num,  int check, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
	// eax                - syscall number
	// edx, ecx, ebx, edi - arg1, arg2, arg3, arg4
	// esi                - return pc
	// ebp                - return esp
	int32_t ret;
	asm volatile("pushl %%ebp;"
				 "pushl %%esi;"
				 "movl %%esp, %%ebp;"
				 "leal after_sysenter_label_%=, %%esi;"
				 "sysenter;"
				 "after_sysenter_label_%=:;"
				 "popl %%esi;"
				 "popl %%ebp;\n"
		     : "=a" (ret)
			 : "a" (num),
		       "d" (a1),
		       "c" (a2),
		       "b" (a3),
		       "D" (a4)
		     : "cc", "memory");

	if(check && ret > 0)
		panic("syscall %d returned %d (> 0)", num, ret);

	return ret;
}

static inline int32_t
syscall(int num, int check, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	int32_t ret;

	// Generic system call: pass system call number in AX,
	// up to five parameters in DX, CX, BX, DI, SI.
	// Interrupt kernel with T_SYSCALL.
	//
	// The "volatile" tells the assembler not to optimize
	// this instruction away just because we don't use the
	// return value.
	//
	// The last clause tells the assembler that this can
	// potentially change the condition codes and arbitrary
	// memory locations.

	asm volatile("int %1\n"
		     : "=a" (ret)
		     : "i" (T_SYSCALL),
		       "a" (num),
		       "d" (a1),
		       "c" (a2),
		       "b" (a3),
		       "D" (a4),
		       "S" (a5)
		     : "cc", "memory");

	if(check && ret > 0)
		panic("syscall %d returned %d (> 0)", num, ret);

	return ret;
}

void
sys_cputs(const char *s, size_t len)
{
#ifdef USE_SYSENTER
	sysenter(SYS_cputs, 0, (uint32_t)s, len, 0, 0);
#else
	syscall(SYS_cputs, 0, (uint32_t)s, len, 0, 0, 0);
#endif
}

int
sys_cgetc(void)
{
#ifdef USE_SYSENTER
	return sysenter(SYS_cgetc, 0, 0, 0, 0, 0);
#else
	return syscall(SYS_cgetc, 0, 0, 0, 0, 0, 0);
#endif
}

int
sys_env_destroy(envid_t envid)
{
#ifdef USE_SYSENTER
	return sysenter(SYS_env_destroy, 1, envid, 0, 0, 0);
#else
	return syscall(SYS_env_destroy, 1, envid, 0, 0, 0, 0);
#endif
}

envid_t
sys_getenvid(void)
{
#ifdef USE_SYSENTER
	return sysenter(SYS_getenvid, 0, 0, 0, 0, 0);
#else
	 return syscall(SYS_getenvid, 0, 0, 0, 0, 0, 0);
#endif
}

void
sys_yield(void)
{
#ifdef USE_SYSENTER
	sysenter(SYS_yield, 0, 0, 0, 0, 0);
#else
	syscall(SYS_yield, 0, 0, 0, 0, 0, 0);
#endif
}

int
sys_page_alloc(envid_t envid, void *va, int perm)
{
#ifdef USE_SYSENTER
	return sysenter(SYS_page_alloc, 1, envid, (uint32_t) va, perm, 0);
#else
	return syscall(SYS_page_alloc, 1, envid, (uint32_t) va, perm, 0, 0);
#endif
}

int
sys_page_map(envid_t srcenv, void *srcva, envid_t dstenv, void *dstva, int perm)
{
	return syscall(SYS_page_map, 1, srcenv, (uint32_t) srcva, dstenv, (uint32_t) dstva, perm);
}

int
sys_page_unmap(envid_t envid, void *va)
{
#ifdef USE_SYSENTER
	return sysenter(SYS_page_unmap, 1, envid, (uint32_t) va, 0, 0);
#else
	return syscall(SYS_page_unmap, 1, envid, (uint32_t) va, 0, 0, 0);
#endif
}

// sys_exofork is inlined in lib.h

int
sys_env_set_status(envid_t envid, int status)
{
#ifdef USE_SYSENTER
	return sysenter(SYS_env_set_status, 1, envid, status, 0, 0);
#else
	return syscall(SYS_env_set_status, 1, envid, status, 0, 0, 0);
#endif
}

int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
#ifdef USE_SYSENTER
	return sysenter(SYS_env_set_trapframe, 1, envid, (uint32_t)tf, 0, 0);
#else
	return syscall(SYS_env_set_trapframe, 1, envid, (uint32_t) tf, 0, 0, 0);
#endif
}

int
sys_env_set_pgfault_upcall(envid_t envid, void *upcall)
{
#ifdef USE_SYSENTER
	return sysenter(SYS_env_set_pgfault_upcall, 1, envid, (uint32_t) upcall, 0, 0);
#else
	return syscall(SYS_env_set_pgfault_upcall, 1, envid, (uint32_t) upcall, 0, 0, 0);
#endif
}

int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, int perm)
{
#ifdef USE_SYSENTER
	return sysenter(SYS_ipc_try_send, 0, envid, value, (uint32_t) srcva, perm);
#else
	return syscall(SYS_ipc_try_send, 0, envid, value, (uint32_t) srcva, perm, 0);
#endif
}

int
sys_ipc_recv(void *dstva)
{
#ifdef USE_SYSENTER
	return sysenter(SYS_ipc_recv, 1, (uint32_t)dstva, 0, 0, 0);
#else
	return syscall(SYS_ipc_recv, 1, (uint32_t)dstva, 0, 0, 0, 0);
#endif
}

int
sys_exec(const char *pathname, const char *argv[])
{
#ifdef USE_SYSENTER
	return sysenter(SYS_exec, 1, (uint32_t)pathname, (uint32_t)argv, 0, 0);
#else
	return syscall(SYS_exec, 1, (uint32_t)pathname, (uint32_t)argv, 0, 0, 0);
#endif
}

unsigned int
sys_time_msec(void)
{
#ifdef USE_SYSENTER
	return sysenter(SYS_time_msec, 0, 0, 0, 0, 0);
#else
	return (unsigned int) syscall(SYS_time_msec, 0, 0, 0, 0, 0, 0);
#endif
}

int
sys_net_try_send(const uint8_t* buf, size_t length)
{
#ifdef USE_SYSENTER
	return sysenter(SYS_net_try_send, 1, (uint32_t)buf, length, 0, 0);
#else
	return syscall(SYS_net_try_send, 1, (uint32_t)buf, length, 0, 0, 0);
#endif
}
