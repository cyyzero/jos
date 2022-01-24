// System call stubs.

#include <inc/syscall.h>
#include <inc/lib.h>

#define USE_SYSENTER

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

