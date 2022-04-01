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

---

```c
 // Check that the block we read was allocated. (exercise for
 // the reader: why do we do this *after* reading the block
 // in?)
 if (bitmap && block_is_free(blockno))
  panic("reading free block %08x\n", blockno);
```

把block读进内存后，再判断这块block是否是被分配的。这是因为bitmap也是磁盘上，第一次访问bitmap的时候会触发内存缺页中断，然后把它读进内存后。如果先判断这块block是否是被分配的，那么会导致访问bitmap递归缺页中断。

---

```c
#define MAXOPEN  1024
#define FILEVA  0xD0000000

void
serve_init(void)
{
    int i;
    uintptr_t va = FILEVA;
    for (i = 0; i < MAXOPEN; i++) {
        opentab[i].o_fileid = i;
        opentab[i].o_fd = (struct Fd*) va;
        va += PGSIZE;
    }
}
```

FS进程中，存放Fd数组的内存位于[0xd0000000, 0xd0400000)。在运行过程中，FS进程会分配Fd数组，然后通过IPC映射给用户进程。为了从客户端向服务器发送请求，使用 32 位数字作为请求类型，并将请求的参数存储在联合`Fsipc`中通过IPC共享的页面。在客户端，使用`fsipcbuf`共享页面；在服务器端，将传入请求页面映射到`fsreq`(0x0ffff000)。客户端共享FS传回Fd结构，共有32个，每个占一页，在[0xd0000000, 0xd0020000)。

---

fsipcbuf实际持有的是用户程序，FS只负责接收映射。

```c
// Virtual address at which to receive page mappings containing client requests.
union Fsipc *fsreq = (union Fsipc *)0x0ffff000;
```

`write`时，数据流动方向为：

```
        user env                  |                  FS
buf  ->  fsipcbuf.write.req_buf   =>   fsreq->write.req_buf  ->  disk
```

`read`时，数据流动方向为：

```
  user env      |          FS
fsipcbuf.read   =>   fsreq->read

                FS                |               user env
disk  ->  fsreq->readRet.ret_buf  =>  fsipcbuf.readRet.ret_buf  ->   buf
```

---

在 QEMU 中，在图形窗口中输入的输入显示为从键盘到JOS的输入，而输入到控制台的输入显示为串行端口上的字符。前者对应IRQ_KBD，后者对应IRQ_SERIAL。

---

文件系统位于磁盘1（区别于存放bootloader和kernel的磁盘0）。磁盘利用obj/fs/fs.img文件做模拟，构建的时候会使用fs/fsformat.c程序来格式化。一个Block和页大小一致，占8个sector。超级块位于block 1。

![磁盘分布](https://pdos.csail.mit.edu/6.828/2018/labs/lab5/disk.png)

---

spawn流程：

1. 创建一个env结构体
2. 初始化子进程的栈。父进程在自己的UTEMP页分配页，并且设置好argc和argv，然后把这一页映射给子进程的栈地址。
3. 解析ELF文件，把需要的segment都加载道子进程。也是通过UTEMP来间接读到内存并映射给子进程。
4. 映射父子进程共享的页，具体为父进程标记为share bit的页映射到子进程。
5. 设置子进程的寄存器。主要是设置EIP。
6. 设置子进程状态为runnable。

---

pipe工作时，会给两个FD添加额外的data页，它们映射到同一个物理页。读写时通过这个共享的页来实现。

---

challenge: 实现unix的exec系统调用。

首先，添加了sys_exec系统调用。函数签名为
```c
int sys_exec(const char* pathname, const char*argv[])
```

需要加载程序的段进入内存，这些逻辑无法由用户态完成，因为在执行时会替换自己的进程映像。在内核态完成的话，需要涉及与FS的通信，也面临一些问题。内核态通信无法自旋等待，这样会导致占用kernel_lock不退出，其余程序永远无法陷入内核。内核态通过sys_yield切换也无法保存内核态的上下文（因为不是每个进程有单独的内核栈，而且TF里只保存了用户态的状态）。

所以，替换进程映像的工作主要交给FS来完成。FS新增一个`FSREQ_LOAD`请求，能够针对发送来的进程，替换它的代码段、数据段等。

执行逻辑如下。

1. 用户态程序调用`exec`，会通过`sys_exec`陷入内核。
2. 如果内核测试FS不还未在recving状态，就重新调整EIP指向int指令，sys_yield切出。
3. 标记当前进程状态为`ENV_NOT_RUNNABLE`
4. 内核申请`UTEMP`一页，作为传给FS的fsipc页。在里面放入`pathname`。
5. 内核根据argv初始化程序的栈。
6. 内核清理用户态的内存也，除了用户态栈和标记为share的页（因为要共享FD）。
7. 步骤2保证了当前FS在recving阶段。所以直接`ipc_send`。内核直接退出，当前进程不会接受FS的返回。如果FS加载失败，会直接把当前进程杀死。（这个语义和Unix不一样）。
8. FS接受了发来的`FSREQ_LOAD`请求，把pathname拷贝到栈上，通过`sys_page_unmap`帮助对端进程释放`UTEMP`，
9. 根据pathname寻找文件位置。
10. 读取文件的ELF_header，判断MAGIC number，并读取program header。
11. 遍历program header，读取这些段，并加载进对端进程的内存。
12. 设置对端进程的eip为ELF header里的entry。并设置对端进程为`ENV_RUNNABLE`。
13. 如果在FS端，上述流程没成功，就直接`sys_destroy`销毁对端进程。

上述的实现需要允许FS对普通进程执行许多操作，如`sys_page_map`\`sys_page_unmap`\`sys_env_set_trapframe`等等。
