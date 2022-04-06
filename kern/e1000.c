#include <kern/e1000.h>
#include <kern/pmap.h>
#include <kern/pci.h>

// LAB 6: Your driver code here

volatile uint8_t* e1000_io_base;

int
e1000_attach(struct pci_func *f)
{
    pci_func_enable(f);
    e1000_io_base = mmio_map_region(f->reg_base[0], f->reg_size[0]);
    assert(*(uint32_t*)(e1000_io_base + E1000_STATUS) == 0x80080783);
    cprintf("e1000 status : 0x%x\n", *(uint32_t*)(e1000_io_base + E1000_STATUS));
    return 1;
}
