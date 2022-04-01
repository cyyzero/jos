#include <inc/lib.h>

void
umain(int argc, char **argv)
{
    int r;
	cprintf("i am original environment %08x\n", thisenv->env_id);
	if ((r = execl("ls", "ls", 0)) < 0)
		panic("spawn(hello) failed: %e", r);
}