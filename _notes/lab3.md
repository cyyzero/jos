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
