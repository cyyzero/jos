// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/pmap.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line

// change permission options
enum { SET_PERM, ADD_PERM, REMOVE_PERM, };

static void print_memory_map(pde_t* pgdir, void* addr);
static void change_permission(pde_t* pgdir, void* addr, int opt, uint32_t bits);

struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Display stack backtrace", mon_backtrace},
	{ "showmappings", "Display memory mapping, format: {begin address} {end addres}", mon_show_mappings},
	{ "changepageperm", "Change page table entry permissions", mon_change_page_perm},
};

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	struct Eipdebuginfo info;
	uintptr_t eip;
	uint32_t *ebp, *args;
	ebp = (uint32_t *)read_ebp();
	cprintf("Stack backtrace:\n");
	do {
		eip = (uintptr_t)*(ebp + 1);
		args = ebp + 2;
		cprintf("  ebp %08x  eip %08x  args %08x %08x %08x %08x %08x\n", ebp, eip, args[0], args[1], args[2], args[3], args[4]);
		
		if (debuginfo_eip(eip, &info) == 0) {
			cprintf("         %s:%d: %.*s+%d\n", info.eip_file, info.eip_line, info.eip_fn_namelen, info.eip_fn_name, eip-info.eip_fn_addr);
		}

		ebp = (uint32_t *)*ebp;
	// in entry.S, ebp initialized to 0
	} while (ebp != 0);
	return 0;
}

int
mon_show_mappings(int argc, char **argv, struct Trapframe *tf)
{
	if (argc < 2 || argc > 3) {
		cprintf("format error, please givin one or two address");
		return 0;
	}
	void *start_addr, *end_addr = 0;
	start_addr = (void*)ROUNDDOWN(atoi(argv[1]), PGSIZE);
	if (argc == 3) {
		end_addr = (void*)ROUNDDOWN(atoi(argv[2]), PGSIZE);
	} else {
		end_addr = start_addr;
	}
	if (end_addr < start_addr) {
		return 0;
	}
	size_t count = ((size_t)(end_addr - start_addr) >> PGSHIFT) + 1;
	log("start: %x end: %x count: %d", start_addr, end_addr, count);
	pde_t* pgdir = (pde_t*)KADDR(rcr3());
	for (size_t i = 0; i < count; ++i, start_addr += PGSIZE) {
		print_memory_map(pgdir, start_addr);
	}
	return 0;
}

int
mon_change_page_perm(int argc, char **argv, struct Trapframe *tf)
{
	if (argc < 2) {
		return 0;
	}
	int opt;
	int idx = 1;
	uint32_t bits = 0;
	void *start_addr, *end_addr;

	// parse change option
	if (strcmp(argv[idx], "--set") == 0) {
		opt = SET_PERM;
	} else if (strcmp(argv[idx], "--add") == 0) {
		opt = ADD_PERM;
	} else if (strcmp(argv[idx], "--remove") == 0) {
		opt = REMOVE_PERM;
	} else {
		idx--;
		// set bits as default
		opt = SET_PERM;
	}
	++idx;

	if (idx == argc) {
		return 0;
	}

	// parse permissions
	for (const char* p = argv[idx]; *p; ++p) {
		switch (*p) {
		case 'u':
		case 'U':
			bits |= PTE_U;
			break;
		case 'w':
		case 'W':
			bits |= PTE_W;
			break;
		default:
			break;
		}
	}
	idx++;
	if (idx == argc) {
		return 0;
	}

	// parse address range
	start_addr = (void*)ROUNDDOWN(atoi(argv[idx++]), PGSIZE);
	if (idx < argc) {
		end_addr = (void*)ROUNDDOWN(atoi(argv[idx]), PGSIZE);
	} else {
		end_addr = start_addr;
	}
	pde_t *pgdir = (pde_t*)KADDR(rcr3());
	size_t count = ((end_addr - start_addr) >> PGSHIFT) + 1;
	for (size_t i = 0; i < count; ++i, start_addr += PGSIZE) {
		log("change perm: addr %p, opt: %d, bits: %x", start_addr, opt, bits);
		change_permission(pgdir, start_addr, opt, bits);
		print_memory_map(pgdir, start_addr);
	}
	return 0;
}


/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");


	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}

static void
print_memory_map(pde_t* pgdir, void* addr)
{
	pde_t *pde = &pgdir[PDX(addr)];
	uint32_t flag = 0;
	if (!(*pde & PTE_P)) {
		return;
	}
	pte_t* page_table = (pte_t*)KADDR(PTE_ADDR(*pde));
	pte_t* pte = &page_table[PTX(addr)];
	if (!(*pte & PTE_P)) {
		return;
	}
	flag = *pte & *pde;
	log("pde: %x  pte: %x", *pde, *pte);
	cprintf("0x%08x\t0x%08x\t", addr, PTE_ADDR(*pte));
	if (flag & PTE_G) {
		cprintf(" G");
	}
	if (flag & PTE_PS) {
		cprintf(" PS");
	}
	if (flag & PTE_D) {
		cprintf(" D");
	}
	if (flag & PTE_A) {
		cprintf(" A");
	}
	if (flag & PTE_PCD) {
		cprintf(" PCD");
	}
	if (flag & PTE_PWT) {
		cprintf(" PWT");
	}
	if (flag & PTE_U) {
		cprintf(" U");
	}
	if (flag & PTE_W) {
		cprintf(" W");
	}
	if (flag & PTE_P) {
		cprintf(" P");
	}
	cprintf("\n");
}

static void change_permission(pde_t* pgdir, void* addr, int opt, uint32_t bits)
{
	pde_t *pde = &pgdir[PDX(addr)];
	if (!(*pde & PTE_P)) {
		return;
	}
	pte_t *page_table = (pte_t*)KADDR(PTE_ADDR(*pde));
	pte_t *pte = &page_table[PTX(addr)];
#define PTE_SET_PERM(pte, bit) \
	if (bits & bit) \
		*pte |= (bit); \
	else \
		*pte &= ~((pte_t)(bit));

#define PTE_ADD_PERM(pte, bit) \
	if (bits & (bit)) \
		*pte |= ((bit));

#define PTE_REMOVE_PERM(pte, bit) \
	if (bits & bit) \
		*pte &= ~((pte_t)(bit));
	log("pde: %x ,  pte: %x , opt: %d, bits: %x", *pde, *pte, opt, bits);

	switch (opt) {
	case SET_PERM:
		PTE_SET_PERM(pte, PTE_W);
		PTE_SET_PERM(pte, PTE_U);
		break;
	case ADD_PERM:
		PTE_ADD_PERM(pte, PTE_W);
		PTE_ADD_PERM(pte, PTE_U);
		break;
	case REMOVE_PERM:
		PTE_REMOVE_PERM(pte, PTE_W);
		PTE_REMOVE_PERM(pte, PTE_U);
		break;
	default:
		break;
	}
}
