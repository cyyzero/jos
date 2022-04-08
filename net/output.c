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

		if ((rsq = ipc_recv(&from, (void*)&nsipcbuf, NULL)) < 0) {
			cprintf("ns_output recv from ns failed, fromID 0x%x, error %e\n", from, rsq);
			continue;
		}
		if (rsq != NSREQ_OUTPUT) {
			cprintf("ns_output recv unsupported rsq type: %x\n", rsq);
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
