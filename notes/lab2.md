#lab 2

entry.S中，临时的虚实地址映射为[0, 4M) => [0, 4M)， [0xF0000000, 0xF0000000+4M) => [0, 4M)。4M正好是一个page directory映射的内存大小。

![低地址物理内存映射](img/memory1.png)
![高地址物理内存映射](img/memory2.png)

在物理内存中，
- [0, PGSIZE)包含BIOS中断表之类的信息，不可用于分配
- [PGSIZE, 0xA0000)可自由分配
- [0xA0000, 0x100000)为硬件相关的内存，不可分配
- [0x100000, end + boot_allocated_pages)为内核加载的内存地址，不可分配 (别忘记在page_alloc启用前分配的一些页！！)
- [end + boot_allocated_pages, MAX_PADDR]为自由分配

---

空闲页链表free_page_list需要重排，让[0, 4M)的内存优先分配。经过运行测试，有836个空闲页框是在[0, 4M)范围内。这些页在`page_alloc`之后能够正常使用，因为在一开始的`entrypgdir`里已经映射过。
