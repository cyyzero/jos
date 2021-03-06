#include <kern/e1000.h>
#include <kern/pmap.h>
#include <kern/pci.h>
#include <inc/string.h>
#include <inc/error.h>

// LAB 6: Your driver code here

volatile uint8_t* io_base;
struct e1000_tx_desc tx_desc_array[E1000_TX_DESC_N];
char tx_buffer[E1000_TX_DESC_N][E1000_TX_BUFFER_SIZE];
struct e1000_rx_desc rx_desc_array[E1000_RX_DESC_N];
char rx_buffer[E1000_RX_DESC_N][E1000_RX_BUFFER_SIZE];

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

static uint32_t
read_tx_desc_tail()
{
    return read_ctrl_reg32(E1000_TDT);
}

static void
write_tx_desc_tail(uint32_t tail)
{
    write_ctrl_reg32(E1000_TDT, tail);
}

static uint32_t
read_rx_desc_tail()
{
    return read_ctrl_reg32(E1000_RDT);
}

static void
write_rx_desc_tail(uint32_t tail)
{
    write_ctrl_reg32(E1000_RDT, tail);
}

static int
tx_init()
{
    assert((uint32_t)tx_desc_array % 16 == 0);
    for (int i = 0; i < E1000_TX_DESC_N; i++) {
        tx_desc_array[i].addr = PADDR(tx_buffer[i]);
        tx_desc_array[i].status = E1000_TXD_STAT_DD;
    }

    // set Transmit Descriptor Base Address
    write_ctrl_reg32(E1000_TDBAL, PADDR(tx_desc_array));
    write_ctrl_reg32(E1000_TDBAH, 0);
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

static int
rx_init()
{
    for (int i = 0; i < E1000_RX_DESC_N; i++) {
        rx_desc_array[i].addr= PADDR(rx_buffer[i]);
        rx_desc_array[i].status = 0;
    }
    // set receive address with MAC address and set Address Valid bit
    // hard code the mac address 52:54:00:12:34:56 with correct endian
    write_ctrl_reg32(E1000_RAL, 0x12005452);
    write_ctrl_reg32(E1000_RAH, 0x80005634);
    // set receive descriptor registers
    assert((uint32_t)rx_desc_array % 16 == 0);
    write_ctrl_reg32(E1000_RDBAL, PADDR(rx_desc_array));
    write_ctrl_reg32(E1000_RDBAH, 0);
    // set Receive Descriptor Length
    assert(sizeof(rx_desc_array) % 128 == 0);
    write_ctrl_reg32(E1000_RDLEN, sizeof(rx_desc_array));
    // set Head and Tail
    write_ctrl_reg32(E1000_RDH, 0);
    write_ctrl_reg32(E1000_RDT, E1000_RX_DESC_N-1);
    // set Receive Control (RCTL) register
    /// enable receiver
    /// receive broadcast packets
    /// set BSIZE 00, make Receive Buffer Size = 2048 Bytes
    write_ctrl_reg32(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    return 0;
}

int
e1000_send(uint8_t *buf, size_t length)
{
    if (length > E1000_TX_BUFFER_SIZE) {
        return -E_OVER_LENGTH;
    }
    size_t tail = read_tx_desc_tail();
    if (!(tx_desc_array[tail].status & E1000_TXD_STAT_DD)) {
        return -E_FULL_BUFFER;
    }
    memcpy(tx_buffer[tail], buf, length);
    // tx_desc_array[tail].addr is fixed
    tx_desc_array[tail].length = length;
    // set report bit
    tx_desc_array[tail].cmd = E1000_TXD_CMD_RS | E1000_TXD_CMD_EOP;
    // clear is done bit
    tx_desc_array[tail].status = 0;

    // if tail will be out of array, round it to zero
    if (++tail == E1000_TX_DESC_N)
        tail = 0;
    write_tx_desc_tail(tail);
    return 0;
}

int 
e1000_recv(uint8_t *buf, size_t length)
{
    size_t recv_len;
    size_t tail = read_rx_desc_tail();
    if (++tail == E1000_RX_DESC_N)
        tail = 0;
    if (!(rx_desc_array[tail].status & E1000_RXD_STAT_DD)) {
        return -E_FULL_BUFFER;
    }
    // disable long packets
    assert(rx_desc_array[tail].status & E1000_RXD_STAT_EOP);
    if (rx_desc_array[tail].length > length) {
        return -E_BUFFER_TOO_SMALL;
    }

    memcpy(buf, rx_buffer[tail], rx_desc_array[tail].length);
    // clear DD and EOP
    rx_desc_array[tail].status = 0;
    recv_len = rx_desc_array[tail].length;
    write_rx_desc_tail(tail);
    return recv_len;
}

static void
tx_test()
{
    // const size_t len = 64;
    uint8_t buf[64] = "abcdefgh";
    int r;
    for (int i = 0; i < 1000; i++) {
        if ((r = e1000_send((uint8_t*)buf, 64)) < 0) {
            cprintf("%d: send failed, %e\n", i, r);
        }
    }
}

int
e1000_attach(struct pci_func *f)
{
    pci_func_enable(f);
    io_base = mmio_map_region(f->reg_base[0], f->reg_size[0]);
    assert(read_ctrl_reg32(E1000_STATUS) == 0x80080783);
    tx_init();
    cprintf("tx init finish\n");
    rx_init();
    cprintf("rx init finish\n");
    // tx_test();
    return 1;
}
