/* See COPYRIGHT for copyright information. */

#include <inc/mmu.h>
#include <inc/memlayout.h>
#include <inc/trap.h>

#include <kern/picirq.h>


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

#define TRAPHANDLER_INTERNAL(name) TRAPHANDLER(name##_ENTRY, T_##name)

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

#define TRAPHANDLER_INTERNAL_NOEC(name) TRAPHANDLER_NOEC(name##_ENTRY, T_##name)
#define TRAPHANDLER_EXTERNAL_NOEC(name) TRAPHANDLER_NOEC(name##_ENTRY, IRQ_OFFSET+IRQ_##name)

.text

.global sysenter_handler;
.type sysenter_handler @function;
.align 2;
sysenter_handler:
	cli;
	pushl %ebp;
	pushl %esi;
	pushl %edi;
	pushl %ebx;
	pushl %ecx;
	pushl %edx;
	pushl %eax;
	call sysenter_wrapper;
	add $20, %esp;
	popl %edx;
	popl %ecx;
	sti;
	sysexit;
.global sysenter_handler_end;
.type sysenter_handler_end @function;
sysenter_handler_end:
	nop;

/*
 * Generate entry points for the different traps.
 * Which exceptions has error_code? reference: https://wiki.osdev.org/Exceptions
 */
TRAPHANDLER_INTERNAL_NOEC(DIVIDE)
TRAPHANDLER_INTERNAL_NOEC(DEBUG)
TRAPHANDLER_INTERNAL_NOEC(NMI)
TRAPHANDLER_INTERNAL_NOEC(BRKPT)
TRAPHANDLER_INTERNAL_NOEC(OFLOW)
TRAPHANDLER_INTERNAL_NOEC(BOUND)
TRAPHANDLER_INTERNAL_NOEC(ILLOP)
TRAPHANDLER_INTERNAL_NOEC(DEVICE)
TRAPHANDLER_INTERNAL(DBLFLT)
# 9 is reserve
TRAPHANDLER_INTERNAL(TSS)
TRAPHANDLER_INTERNAL(SEGNP)
TRAPHANDLER_INTERNAL(STACK)
TRAPHANDLER_INTERNAL(GPFLT)
TRAPHANDLER_INTERNAL(PGFLT)
# 15 is reserved
TRAPHANDLER_INTERNAL_NOEC(FPERR)
TRAPHANDLER_INTERNAL(ALIGN)
TRAPHANDLER_INTERNAL_NOEC(MCHK)
TRAPHANDLER_INTERNAL_NOEC(SIMDERR)

# 16-47, external interrupts
TRAPHANDLER_EXTERNAL_NOEC(TIMER)
TRAPHANDLER_EXTERNAL_NOEC(KBD)
TRAPHANDLER_EXTERNAL_NOEC(SERIAL)
TRAPHANDLER_EXTERNAL_NOEC(SPURIOUS)
TRAPHANDLER_EXTERNAL_NOEC(IDE)
TRAPHANDLER_EXTERNAL_NOEC(ERROR)

# 48
TRAPHANDLER_INTERNAL_NOEC(SYSCALL)

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
