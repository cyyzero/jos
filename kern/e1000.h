#ifndef JOS_KERN_E1000_H
#define JOS_KERN_E1000_H

#include <kern/pci.h>

#define E1000_STATUS   0x00008  /* Device Status - RO */

extern volatile uint8_t* e1000_io_base;

int e1000_attach(struct pci_func *f);

#endif  // SOL >= 6
