#include "ns.h"

extern union Nsipc nsipcbuf;

void
output(envid_t ns_envid)
{
	binaryname = "ns_output";

	// LAB 6: Your code here:
	// 	- read a packet from the network server
	//	- send the packet to the device driver
	while (1) {
		envid_t from;
		int rsq, r;
		rsq = ipc_recv(&from, (void*)&nsipcbuf, NULL);
		if (rsq != NSREQ_OUTPUT || from != ns_envid) {
			cprintf("rsq is %x, from is %x\n", rsq, from);
			continue;
		}
		int cnt = SEND_RETRY_TIME;
		while (cnt && (r = sys_net_try_send((const uint8_t*)&nsipcbuf.pkt.jp_data, nsipcbuf.pkt.jp_len)) < 0) {
			if (r == -E_FULL_BUFFER) {
				--cnt;
			}
			else {
				cprintf("pkg send failed, %e\n", r);
				break;
			}
		}
	}
}
