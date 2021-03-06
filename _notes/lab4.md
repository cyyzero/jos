# lab 4

多处理器初始化有个MultiProcessor Specification。BIOS在初始化的时候会保存一些多处理相关的结构化数据，OS可以直接读取使用。

---

> Question: Compare kern/mpentry.S side by side with boot/boot.S. Bearing in mind that kern/mpentry.S is compiled and linked to run above KERNBASE just like everything else in the kernel, what is the purpose of macro MPBOOTPHYS? Why is it necessary in kern/mpentry.S but not in boot/boot.S? In other words, what could go wrong if it were omitted in kern/mpentry.S?  
Hint: recall the differences between the link address and the load address that we have discussed in Lab 1.

一开始会把[mpentry_start, mpentry_end]的部分拷贝到`MPENTRY_PADDR`，所以所有需要使用地址的地方，包括加载gdt、跳转，都需要额外注意，链接器不会处理这些地址。

```c
#define MPBOOTPHYS(s) ((s) - mpentry_start + MPENTRY_PADDR)
```

`MPBOOTPHYS`用于计算[mpentry_start, mpentry_end]内的符号在拷贝之后的地址，所以`gdtdesc`、`gdt`、`start32`都需要用到`MPBOOTPHYS`。而boot.S里面链接器默认的工作方式就可以完成工作，直接用符号表里的地址就可以解决地址重定位。如果mpentry.S不使用`MPBOOTPHYS`，链接器默认的重定位，会加载和跳转到拷贝前的位置处。

同样的道理，跳转到`mp_main`也需要特殊处理。

```asm
	# Call mp_main().  (Exercise for the reader: why the indirect call?)
	movl    $mp_main, %eax
	call    *%eax
```

不能直接用`call mp_main`，因为这样的话，`call`的机器码操作数会是一个偏移量，而链接器在计算偏移量是根据的原始位置，而不会是拷贝后的位置。使用`ljmpl   $(PROT_MODE_CSEG), $mp_main`之类的方式也能实现跳转。

---

> Question: It seems that using the big kernel lock guarantees that only one CPU can run the kernel code at a time. Why do we still need separate kernel stacks for each CPU? Describe a scenario in which using a shared kernel stack will go wrong, even with the protection of the big kernel lock.

引发trap后，控制流进入各个trap各自的entry。如果是从用户态到内核态，此时也会进行运行栈的切换。如果两个用户程序分别同时引发了trap，陷入内核，此时还无法进行lock互斥，如果不对内核栈地址进行区分，他们会使用同一个栈。

---

> 3. In your implementation of env_run() you should have called lcr3(). Before and after the call to lcr3(), your code makes references (at least it should) to the variable e, the argument to env_run. Upon loading the %cr3 register, the addressing context used by the MMU is instantly changed. But a virtual address (namely e) has meaning relative to a given address context--the address context specifies the physical address to which the virtual address maps. Why can the pointer e be dereferenced both before and after the addressing switch?

`UTOP`以上的地址空间的页表映射是相同的，所以页表切换前后`e`都可以正常访问。

> 4. Whenever the kernel switches from one environment to another, it must ensure the old environment's registers are saved so they can be restored properly later. Why? Where does this happen?

当中断发生的时候，会通过pushal等操作将寄存器存到栈上的`Trapframe`结构中，然后在`trap`函数中会将这部分数据存到`curenv->env_tf`。切换的时候会通过`pop`和`iret`等操作恢复`env_tf`中的寄存器数据。此时只能恢复通用寄存器的数据，不包括浮点等其它的寄存器。

---

JOS支持用户自定义的page fault处理函数。在处理page fault时，内核会切换到用户态的用户处理函数，包括`cs:ip`和`ss:sp`的切换，`UXSTACKTOP-PGSIZE`这一页作为user exception stack。要防止嵌套page fault，所以需要检查`tf_esp`是否已经在user exception stack，以调整切换后的栈顶。还需要在user exception stack上写入trap前的寄存器状态，以便用户处理完错误后恢复。切换后完全就是用户态，用户可以申请页等方式处理page fault。处理结束后利用一点小技巧，恢复状态并直接跳转回trap前的指令。

---

JOS初始化env的时候，会设置UTOP以上的内核地址空间。实现fork之类的函数，需要复制UTOP以下的地址空间。利用uvpt和uvpd可以直接访问页目录和页表。COW-fork的基本原理：调用fork后，父进程复制用户态的地址空间映射给子进程，可写的页映射成PTE_COW，只读的页则照旧。当处理page fault时，如果出错的地址所在页映射时PTE_COW，就复制这一页，并且出错的地址映射到此页上。

---

```c
#define thisenv (*env)

const volatile struct Env **env;
void
libmain(int argc, char **argv)
{
    const volatile struct Env *stack_env;
    stack_env = &envs[ENVX(eid)];
    env = &stack_env;

    // ...
    // call user main routine
    umain(argc, argv);

    // exit gracefully
    exit();
}
```

实现`sfork`时，全局变量共享，运行栈COW。所以`thisenv`替换成宏，访问栈上的数据，以保证每个进程的`thisenv`是独立的。

---

需要改进对sysenter的支持，如增加kernel全局锁、关闭开启中断等。由于sysenter和cli无法原子执行，有可能在sysenter和cli指令之间，发生时钟中断。这种情况下需要在trap中特殊处理：

```c
extern void sysenter_handler();
extern void sysenter_handler_end();
if (tf->tf_eip >= (uintptr_t)sysenter_handler && tf->tf_eip < (uintptr_t)sysenter_handler_end) {
    if (tf->tf_trapno == IRQ_OFFSET + IRQ_TIMER) {
        env_run(curenv);
        return;
    }
}
```

执行需要切换任务的系统调用，比如`sys_sched_yield`等，需要在`curenv->env_tf`中保存相关状态，这样才能在下次调度到它时正常恢复：

```c
#define STORE_TF \
curenv->env_tf.tf_cs = GD_UT | 3; \
curenv->env_tf.tf_eip = eip; \
curenv->env_tf.tf_ss = GD_UD | 3; \
curenv->env_tf.tf_esp = esp; \
curenv->env_tf.tf_eflags = read_eflags() | FL_IF;
```
