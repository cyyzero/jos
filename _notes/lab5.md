# lab 5

x86保护模式利用两种机制为IO提供了权限控制：

- `EFLAGS`寄存器中的`IOPL`字段定义了使用 I/O 相关指令的权限。
- TSS段的I/O权限位图定义了使用I/O地址空间中的端口的权利。

以下几个指令会涉及IO权限的判断：

- `IN` -- Input
- `INS` -- Input String
- `OUT` -- Output
- `OUTS` -- Output String
- `CLI` -- Clear Interrupt-Enable Flag
- `STI` -- Set Interrupt-Enable

当执行`CLI`和`STI`指令时，会检查CPL <= IOPL，否则会引发GP异常。
当执行`IN` `OUT`这两类指令时，也会检查CPL <= IOPL，符合就继续。否则还会检查TSS的IO比特位。

以`IN`指令为例子，执行逻辑如下：

```c
if(PE == 1 && CPL > IOPL || VM == 1) { //Protected mode with CPL > IOPL or virtual-8086 mode
	if(AnyPermissionBitSet(CurrentIOPort())) Exception(GP(0)); //If any I/O Permission Bit for I/O port being accessed == 1 the I/O operation is not allowed
	else Destination = Source; //I/O operation is allowed; Reads from selected I/O port
}
else Destination = Source; //Real Mode or Protected Mode with CPL <= IOPL; Reads from selected I/O port
```

---

> Do you have to do anything else to ensure that this I/O privilege setting is saved and restored properly when you subsequently switch from one environment to another? Why? 

不需要额外操作，从CPL = 0的环境iret时，允许EFLAG恢复IOPL。
