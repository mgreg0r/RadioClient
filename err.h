#ifndef _ERR_
#define _ERR_

/* Prints information about incorrect execution of system function
and terminates application */
extern void syserr(const char *fmt, ...);

// Prints information about error and terminates application
extern void fatal(const char *fmt, ...);

#endif
