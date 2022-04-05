#include <kern/e1000.h>
#include <kern/pmap.h>
#include <kern/pci.h>

// LAB 6: Your driver code here

int
e1000_attach(struct pci_func *f)
{
    pci_func_enable(f);

    return 1;
}
