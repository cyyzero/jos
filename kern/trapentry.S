/* See COPYRIGHT for copyright information. */

#include <inc/mmu.h>
#include <inc/memlayout.h>
#include <inc/trap.h>



###################################################################
# exceptions/interrupts
###################################################################

/* TRAPHANDLER defines a globally-visible function for handling a trap.
 * It pushes a trap number onto the stack, then jumps to _alltraps.
 * Use TRAPHANDLER for traps where the CPU automatically pushes an error code.
 *
 * You shouldn't call a TRAPHANDLER function from C, but you may
 * need to _declare_ one in C (for instance, to get a function pointer
 * during IDT setup).  You can declare the function with
 *   void NAME();
 * where NAME is the argument passed to TRAPHANDLER.
 */
#define TRAPHANDLER(name, num)						\
	.globl name;		/* define global symbol for 'name' */	\
	.type name, @function;	/* symbol type is function */		\
	.align 2;		/* align function definition */		\
	name:			/* function starts here */		\
	pushl $(num);							\
	jmp _alltraps

#define TH(name) TRAPHANDLER(name##_ENTRY, T_##name)

/* Use TRAPHANDLER_NOEC for traps where the CPU doesn't push an error code.
 * It pushes a 0 in place of the error code, so the trap frame has the same
 * format in either case.
 */
#define TRAPHANDLER_NOEC(name, num)					\
	.globl name;							\
	.type name, @function;						\
	.align 2;							\
	name:								\
	pushl $0;							\
	pushl $(num);							\
	jmp _alltraps

#define TH_N(name) TRAPHANDLER_NOEC(name##_ENTRY, T_##name)

.text

.global sysenter_handler;
.type sysenter_handler @function;
.align 2;
sysenter_handler:
	pushl %ebp;
	pushl %esi;
	pushl $0;
	pushl %edi;
	pushl %ebx;
	pushl %ecx;
	pushl %edx;
	pushl %eax;
	call syscall;
	add $24, %esp;
	popl %edx;
	popl %ecx;
	sysexit;

/*
 * Generate entry points for the different traps.
 * Which exceptions has error_code? reference: https://wiki.osdev.org/Exceptions
 */
TH_N(DIVIDE)
TH_N(DEBUG)
TH_N(NMI)
TH_N(BRKPT)
TH_N(OFLOW)
TH_N(BOUND)
TH_N(ILLOP)
TH_N(DEVICE)
TH(DBLFLT)
# 9 is reserve
TH(TSS)
TH(SEGNP)
TH(STACK)
TH(GPFLT)
TH(PGFLT)
# 15 is reserved
TH_N(FPERR)
TH(ALIGN)
TH_N(MCHK)
TH_N(SIMDERR)

# 48
TH_N(SYSCALL)

_alltraps:
    # Push values to make the stack look like a struct Trapframe
	pushl %ds
	pushl %es
	pushal
	# load GD_KD into %ds and %es
	movw $GD_KD, %ax
	movw %ax, %ds
	movw %ax, %es
	# pass a pointer to the Trapframe as an argument to trap()
	pushl %esp
	call trap

