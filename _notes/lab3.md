# lab3

需要在初始化内存阶段预分配进程结构的数组 `envs`，一共有1024个 `Env`结构体。分配之后，[0, 4M)物理内存区域里还剩下692个空闲页框。

分配后的映射关系如下图：

| virtual address         | physical address        | size  | flag  | remark                           |
| ----------------------- | ----------------------- | ----- | ----- | -------------------------------- |
| 0xf0000000 - 0xffffffff | 0x00000000 - 0x0fffffff | 256 M | W P   | 涵盖了目前所有的物理内存（128M） |
| 0xefff8000 - 0xefffffff | 0x00110000 - 0x00117fff | 32K   | W P   | 映射了内核的stack                |
| 0xef000000 - 0xef03ffff | 0x0011d000 - 0x0014ffff | 256K  | W P   | 映射了pags结构数组               |
| 0xeec00000 - 0xeec17fff | 0x001d3000 - 0x001eafff | 96K   | W U P | 映射了envs结构数组               |

---

env初始化过程中重新加载了GDT，加入了用户态的code和data段，也采用了平坦模型。

---

由于暂时没有文件系统，链接器会以`-b binary`选项，将用户程序作为binary放进kernel ELF的data段，然后在符号表里对于每个用户程序会有如下的信息：

```
00008acc A _binary_obj_user_hello_size
f011c330 D _binary_obj_user_hello_start
f0124dfc D _binary_obj_user_buggyhello_start
```

---

利用`iret`指令实现了从0特权的内核到3特权的用户态。[`iret`在32位保护模式的流程概要为](https://www.felixcloutier.com/x86/iret:iretd)：

```asm
// 切换EIP CS
EIP <- Pop()
CS <- Pop()      ; (* 32-bit pop, high-order 16 bits discarded *)
tempEFLAGS  <- Pop()

// 跳转到低特权级
if CS.RPL > CPL:
    // 跳转到不同特权级，需要切换ESP SS
    ESP <- Pop()
    SS <- Pop()
    // 切换EFLAGS
    EFLAGS (CF, PF, AF, ZF, SF, TF, DF, OF, NT, RF, AC, ID) ← tempEFLAGS;
    IF CPL <= IOPL:
        EFLAGS(IF) <- tempEFLAGS
    IF CPL == 0:
        EFLAGS(IOPL) <- tempEFLAGS
        EFLAGS(VIF, VIP) <- tempEFLAGS
    // 切换CPL
    CPL <- CS.RPL
    // 检查其他段寄存器的权限
    for seg in ES, FS, GS, and DS:
        if DPL < CPL:
            seg = null

// 相同特权级
if CS.RPL == CPL:
    // EFLAGS的切换和上面一致
    EFLAGS (CF, PF, AF, ZF, SF, TF, DF, OF, NT, RF, AC, ID) ← tempEFLAGS
    IF CPL <= IOPL:
        EFLAGS(IF) <- tempEFLAGS
    IF CPL == 0:
        EFLAGS(IOPL) <- tempEFLAGS
        EFLAGS(VIF, VIP) <- tempEFLAGS

// 不允许跳转到高特权
if CS.RPL < CPL:
    raise #GP exception
```

---

Q1: What is the purpose of having an individual handler function for each exception/interrupt? (i.e., if all exceptions/interrupts were delivered to the same handler, what feature that exists in the current implementation could not be provided?)

A: 需要给每个中断设置独立的handler，因为触发中断的时候cpu不会自动存储中断号。如果统一了handler，那handler就无法确定自己是由哪个中断所触发。

Q2: Did you have to do anything to make the user/softint program behave correctly? The grade script expects it to produce a general protection fault (trap 13), but softint's code says int $14. Why should this produce interrupt vector 13? What happens if the kernel actually allows softint's int $14 instruction to invoke the kernel's page fault handler (which is interrupt vector 14)?

A: softint程序手动引发`int $14`指令，14号是page fault中断，理论上不应该由用户态主动跳转。`int`指令会检查IDT表项的DPL字段（硬件触发的中断不用检查这一项），14号的IDT表项DPL为0，不允许用户触发，所以会引发GP异常，跳到13号中断处理程序中（13号中断处理专门负责GP异常）。

---

[用户态（CPL == 3）执行`int`指令在32位保护模式的流程概要为](https://www.felixcloutier.com/x86/intn:into:int3:int1)：

```
// 防止跳转到低特权的代码段，一般都是用于从CPL=3跳转到 DPL = 0的代码段
// 所以以下也都是默认代码段DPL < CPL的流程，涉及到栈的切换。
IF new code-segment DPL > CPL
    raise #GP

// 确保中断描述符的DPL权限等于CPL 3，所以一般只有少数的一些中断允许用户主动触发，比如系统调用
// 能够解释上述问题Q2。防止低特权级程序利用软中断触发任意中断。
IF gate DPL < CPL (* PE = 1, DPL < CPL, software interrupt *)
    raise #GP

// 从TSS中加载SS和ESP
NewSS <- TSS.SS
NewESP <- TSS.ESP
// 中断描述符中有个segment descriptor，指向一个描述符表项，它的DPL要和NewSS.RPL、new stack-segment DPL相同。一般这三个都是0。
NewSS.RPL != new code-segment DPL || new stack-segment DPL != new code-segment DPL
    raise #TS

SS <- NewSS
ESP <- NewESP

// 保证切换后的栈空间够用
 IF new stack does not have room for 24 bytes (error code pushed)
        or 20 bytes (no error code pushed)
    raise #SS

// 切换CS:IP, 从IDT项里面加载
CS:EIP <- Gate(CS:EIP); (* Segment descriptor information also loaded *)

// 新的栈上压入数据
// 和iret的pop顺序正好相反
Push(SS, ESP)
Push(EFLAGS)
Push(CS, EIP)
// 如果有错误码的话
( Push(ErrorCode))

// 设置CPL，EFLAGS
CPL <- new code-segment DPL
CS(RPL) <- CPL
// interrupt会关闭IF，IF会屏蔽硬件中断
IF IDT gate is interrupt gate
    IF <- 0 (* Interrupt flag set to 0, interrupts disabled *)
TF <- 0;
VM <- 0;
RF <- 0;
NT <- 0;
```

---

注意，用户态`int`软中断后没有改`cr3`，所以页表需要给内核设置好正确权限的地址。

---

`boot.S`里面`CLI`关中断了。

---

Q3: The break point test case will either generate a break point exception or a general protection fault depending on how you initialized the break point entry in the IDT (i.e., your call to SETGATE from trap_init). Why? How do you need to set it up in order to get the breakpoint exception to work as specified above and what incorrect setup would cause it to trigger a general protection fault?
A: 仍然同上面Q2原理一样，陷阱门描述符的DPL设置为3才能在用户态通过`int3`触发，否则引发GP。

Q4: What do you think is the point of these mechanisms, particularly in light of what the user/softint test program does?
A: 意义在于控制用户能主动触发哪些中断。

---

`sysenter`和`sysexit`是用来快速陷入内核态的指令。使用这两个指令要求采用平坦模型，并且GDT设置成以下顺序：

1. ring 0 code segment
2. ring 0 data segment
3. ring 3 code segment
4. ring 3 data segment

`sysenter`通过对MSR的写入来预先设定好进入内核态的栈和指令地址。具体来说，MSR中`IA32_SYSENTER_CS (0x174)`为跳转后的`CS`，`SS`自动为`CS`+8，不需要额外指定（所以需要按照上面的GDT顺序来设置）。`IA32_SYSENTER_ESP (0x175)`为跳转后的栈`ESP`，`IA32_SYSENTER_EIP (0x176)`为跳转后的指令地址`EIP`。跳转之后只改变上述设置的四个寄存器，既`CS:EIP`和`SS:ESP`；和一些EFLAG标志位，`EFLAGS.IF`=0, `EFLAGS.VM`=0, `EFLAGS.RF`=0。其他寄存器一律不变，而且也不会自动在栈上压入数据。这都是为了让跳转速度尽量快。

简化之后的流程如下。实际上，为了高速，`CS`和`SS`都不会从GDT中加载，而是会使用平坦模型的默认值。软件开发者需要保证GDT中为平坦模型，并且顺序和上方描述的一致。

```c
CS  = IA32_SYSENTER_CS
EIP = IA32_SYSENTER_EIP
SS  = IA32_SYSENTER_CS + 8
ESP = IA32_SYSENTER_ESP
EFLAGS.IF = 0, EFLAGS.VM = 0, EFLAGS.RF = 0
```

`sysexit`也是会改变栈和指令地址。`CS`为`IA32_SYSENTER_CS+16`，`EIP`为`EDX`，`SS`为`IA32_SYSENTER_CS+24`，`ESP`为`ECX`。

简化后的流程如下：

```c
CS  = IA32_SYSENTER_CS + 16
EIP = EDX
SS  = IA32_SYSENTER_CS + 24
ESP = ECX
```

所以一般来说，用户态的调用者需要提供好返回所需要的`EIP`和`ESP`。

JOS中可以利用这两个指令来实现快速系统调用，所有需要的参数都通过寄存器传递：

```
	eax                - syscall number
	edx, ecx, ebx, edi - arg1, arg2, arg3, arg4
	esi                - return pc
	ebp                - return esp
```

由于通用寄存器数量的限制，无法将系统调用的5个参数都通过寄存器传递。目前只支持4个参数的系统调用，想要更多参数可以通过传地址的方式来实现。

---

gcc内联汇编中‘%=’输出一个数字，该数字对整个编译中的 asm 语句的每个实例都是唯一的。当创建本地标签并在生成多个汇编程序指令的单个模板中多次引用它们时，比如inline展开的内联汇编，此选项很有用。这种情况下，如果不给标签加`%=`做单独区分，会无法编译。

