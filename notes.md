# 笔记

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

醉了，还碰上了gcc的bug。gcc优化把跳出循环的判断跳过了，直接死循环。我选择了用`__attribute__((optimize("O0")))`禁止对这个函数优化。
