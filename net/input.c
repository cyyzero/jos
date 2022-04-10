#include "ns.h"

extern union Nsipc nsipcbuf;

void sleep(int ms)
{
	int now;
	now = sys_time_msec();
	assert(now > 0);
	while ((sys_time_msec() - now) < ms) {
		sys_yield();
	}
}

void
input(envid_t ns_envid)
{
	binaryname = "ns_input";

	envid_t from;
	int rsq, r;
	int recv_len;

	// LAB 6: Your code here:
	// 	- read a packet from the device driver
	//	- send it to the network server
	// Hint: When you IPC a page to the network server, it will be
	// reading from it for a while, so don't immediately receive
	// another packet in to the same physical page.

	// pre-copy before write to avoid page fault in kernel
	sys_page_alloc(0, &nsipcbuf, PTE_P | PTE_U | PTE_W);
	while (1) {
		for (int i = 0; i < RECV_RETRY_TIME; i++) {
			// cprintf("jp_data %p, uvpt %x\n", nsipcbuf.pkt.jp_data, uvpt[PGNUM(&nsipcbuf)]);
			recv_len = sys_net_try_recv((uint8_t*)nsipcbuf.pkt.jp_data, sizeof(nsipcbuf));
			if (recv_len == -E_FULL_BUFFER) {
				sys_yield();
				continue;
			} else	if (recv_len >= 0) {
				cprintf("receive , len = %d\n", recv_len);
				break;
			}
			else {
				cprintf("recv failed, %e", recv_len);
				nsipcbuf.pkt.jp_len = 0;
			}
		}
		if (recv_len > 0) {
			nsipcbuf.pkt.jp_len = recv_len;
			ipc_send(ns_envid, NSREQ_INPUT, &nsipcbuf, PTE_P | PTE_U | PTE_W);
		}
		sleep(50);
	}
}
