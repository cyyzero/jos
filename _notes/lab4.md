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

```
Question: It seems that using the big kernel lock guarantees that only one CPU can run the kernel code at a time. Why do we still need separate kernel stacks for each CPU? Describe a scenario in which using a shared kernel stack will go wrong, even with the protection of the big kernel lock. 
```

引发trap后，控制流进入各个trap各自的entry。如果是从用户态到内核态，此时也会进行运行栈的切换。如果两个用户程序分别同时引发了trap，陷入内核，此时还无法进行lock互斥，如果不对内核栈地址进行区分，他们会使用同一个栈。
