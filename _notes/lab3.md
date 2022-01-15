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
    CPL ← new code-segment DPL
    CS(RPL) ← CPL
    // interrupt会关闭IF，IF会屏蔽硬件中断
    IF IDT gate is interrupt gate
        IF <- 0 (* Interrupt flag set to 0, interrupts disabled *)
    TF ← 0;
    VM ← 0;
    RF ← 0;
    NT ← 0;
```

---

注意，用户态`int`软中断后没有改`cr3`，所以页表需要给内核设置好正确权限的地址。

---

`boot.S`里面`CLI`关中断了。


