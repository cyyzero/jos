#include <kern/e1000.h>
#include <kern/pmap.h>
#include <kern/pci.h>

// LAB 6: Your driver code here

volatile uint8_t* io_base;
struct e1000_tx_desc tx_desc_array[E1000_TX_DESC_N];
char tx_buffer[E1000_TX_DESC_N][E1000_TX_BUFFER_SIZE];

#define gen_read_ctrl_reg_func(num) \
    static uint##num##_t \
    read_ctrl_reg##num(uint32_t offset) \
    { \
        return *(uint##num##_t*)(io_base + offset); \
    }

#define gen_write_ctrl_reg_func(num) \
    static void \
    write_ctrl_reg##num(uint32_t offset, uint##num##_t val) \
    { \
        *(uint##num##_t*)(io_base + offset) = val;\
    }

#define gen_rw_ctrl_reg_func(num) \
    gen_read_ctrl_reg_func(num) \
    gen_write_ctrl_reg_func(num)

gen_rw_ctrl_reg_func(8)
gen_rw_ctrl_reg_func(16)
gen_rw_ctrl_reg_func(32)
gen_rw_ctrl_reg_func(64)

static int
tx_init()
{
    assert((uint32_t)tx_desc_array % 16 == 0);
    for (int i = 0; i < E1000_TX_DESC_N; i++) {
        tx_desc_array[i].addr = PADDR(tx_buffer[i]);
        tx_desc_array[i].length = E1000_TX_BUFFER_SIZE;
    }

    // set Transmit Descriptor Base Address
    write_ctrl_reg32(E1000_TDBAL, (uint32_t)tx_desc_array);
    // set Transmit Descriptor Length. Must be 128-byte aligned.
    assert(sizeof(tx_desc_array) % 128 == 0);
    write_ctrl_reg32(E1000_TDLEN, sizeof(tx_desc_array));
    // set Transmit Descriptor Head and Tail
    write_ctrl_reg16(E1000_TDH, 0);
    write_ctrl_reg16(E1000_TDT, 0);
    // write CTRL Register
    /// Set the Enable (TCTL.EN) bit to 1b for normal operation.
    /// TIPG controls the IPG (Inter Packet Gap) timer for the Ethernet controller.
    write_ctrl_reg32(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT_ETHNET | 
        E1000_TCTL_COLD_FULL_DUPLEX);
    //set Inter Packet Gap timer
    write_ctrl_reg32(E1000_TIPG, E1000_TIPG_VALUE);
    return 0;
}

int
e1000_attach(struct pci_func *f)
{
    pci_func_enable(f);
    io_base = mmio_map_region(f->reg_base[0], f->reg_size[0]);
    assert(read_ctrl_reg32(E1000_STATUS) == 0x80080783);
    cprintf("e1000 status : 0x%x\n", read_ctrl_reg32(E1000_STATUS));
    tx_init();
    return 1;
}
