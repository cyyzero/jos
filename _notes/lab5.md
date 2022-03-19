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