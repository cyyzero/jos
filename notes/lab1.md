# lab1

bootloader起始于0x7c00,512Byte。栈顶也放在0x7c00。

+------------------+  <- 0xFFFFFFFF (4GB)
|      32-bit      |
|  memory mapped   |
|     devices      |
|                  |
/\/\/\/\/\/\/\/\/\/\

/\/\/\/\/\/\/\/\/\/\
|                  |
|      Unused      |
|                  |
+------------------+  <- depends on amount of RAM
|                  |
|                  |
| Extended Memory  |
|                  |
|                  |
+------------------+  <- 0x00100000 (1MB)
|     BIOS ROM     |
+------------------+  <- 0x000F0000 (960KB)
|  16-bit devices, |
|  expansion ROMs  |
+------------------+  <- 0x000C0000 (768KB)
|   VGA Display    |
+------------------+  <- 0x000A0000 (640KB)
|                  |
|    Low Memory    |
|                  |  <- 0x00007c00 (boot loader)
+------------------+  <- 0x00000000

---

C语言参数从右往左压栈，原因之一就是为了支持变长参数列表。

---

```c
// kern/monitor.c
int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	uint32_t eip;
	uint32_t *ebp, *args;
	ebp = (uint32_t *)read_ebp();
	while (true)
	// for (int i = 0; i < 15; ++i)
	{
		eip = *(ebp + 1);
		args = ebp + 2;
		cprintf("ebp %08x\teip %08x\targs %08x %08x %08x %08x %08x\n", ebp, eip, args[0], args[1], args[2], args[3], args[4]);
		// in entry.S, ebp initialized to 0

		if ((uint32_t)ebp == 0)
		{
			break;
		}

		ebp = (uint32_t *)*ebp;
	}
	return 0;
}
```

```asm
// objdump
		// in entry.S, ebp initialized to 0
		if ((uint32_t)ebp == 0)
		{
			break;
		}
		ebp = (uint32_t *)*ebp;
f01008ea:	8b 36                	mov    (%esi),%esi
f01008ec:	83 c4 20             	add    $0x20,%esp
f01008ef:	eb e0                	jmp    f01008d1 <mon_backtrace+0x20>

f01008f1 <monitor>:
	return 0;
}

```

醉了，还碰上了gcc的bug。gcc -O1优化把`if ((uint32_t)ebp == 0)`的判断跳过了，直接死循环。我选择了用`__attribute__((optimize("O0")))`禁止对这个函数优化。

更新： 目测是因为编译器看到`*(ebp + 1)`，所以认为`ebp`不可能为0，于是直接死循环了。这里是我实现有问题，bootloader里的`entry`还没建立完整的栈帧，回溯到它的时候不需要打印。但这个优化还是很迷，没考虑到写内核的特殊情况。
