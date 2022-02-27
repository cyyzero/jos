// Called from entry.S to get us going.
// entry.S already took care of defining envs, pages, uvpd, and uvpt.

#include <inc/lib.h>

extern void umain(int argc, char **argv);

#ifdef SFORK
const volatile struct Env **env;
#else
const volatile struct Env *thisenv;
#endif
const char *binaryname = "<unknown>";

void
libmain(int argc, char **argv)
{
	// set thisenv to point at our Env structure in envs[].
	envid_t eid = sys_getenvid();
#ifdef SFORK
	const volatile struct Env *stack_env;
	stack_env = &envs[ENVX(eid)];
	env = &stack_env;
#else
	thisenv = &envs[ENVX(eid)];
#endif
	// save the name of the program so that panic() can use it
	if (argc > 0)
		binaryname = argv[0];

	// call user main routine
	umain(argc, argv);

	// exit gracefully
	exit();
}

