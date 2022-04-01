#include <inc/lib.h>

int
execv(const char *pathname, const char *argv[])
{
    int r;
    // close_all();
    // // open fd 0 and 1 for the new program
    // opencons();
    // dup(0, 1);
    r = sys_exec(pathname, argv);
    cprintf("exec failed, %e\n", r);
    // should never return
    r = ipc_recv(NULL, NULL, NULL);
    cprintf("exec failed, %e\n", r);
    return r;
}

int
execl(const char *pathname, const char *arg0, ...)
{
	// We calculate argc by advancing the args until we hit NULL.
	// The contract of the function guarantees that the last
	// argument will always be NULL, and that none of the other
	// arguments will be NULL.
	int argc=0;
	va_list vl;
	va_start(vl, arg0);
	while(va_arg(vl, void *) != NULL)
		argc++;
	va_end(vl);

	// Now that we have the size of the args, do a second pass
	// and store the values in a VLA, which has the format of argv
	const char *argv[argc+2];
	argv[0] = arg0;
	argv[argc+1] = NULL;

	va_start(vl, arg0);
	unsigned i;
	for(i=0;i<argc;i++)
		argv[i+1] = va_arg(vl, const char *);
	va_end(vl);
	return execv(pathname, argv);
}