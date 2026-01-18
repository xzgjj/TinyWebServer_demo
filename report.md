# Build & Test Report

**Generated:** 2026-01-18T21:44:03.947061

## Configure Output
```
Skipped
```

## Build Output
```
Skipped
```

## Test Results
## GDB Debug Log
```
Warning: 'set logging on', an alias for the command 'set logging enabled', is deprecated.
Use 'set logging enabled on'.

[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".

Program received signal SIGABRT, Aborted.
__pthread_kill_implementation (no_tid=0, signo=6, threadid=140737352709120) at ./nptl/pthread_kill.c:44
44	./nptl/pthread_kill.c: No such file or directory.
#0  __pthread_kill_implementation (no_tid=0, signo=6, threadid=140737352709120) at ./nptl/pthread_kill.c:44
#1  __pthread_kill_internal (signo=6, threadid=140737352709120) at ./nptl/pthread_kill.c:78
#2  __GI___pthread_kill (threadid=140737352709120, signo=signo@entry=6) at ./nptl/pthread_kill.c:89
#3  0x00007ffff7842476 in __GI_raise (sig=sig@entry=6) at ../sysdeps/posix/raise.c:26
#4  0x00007ffff78287f3 in __GI_abort () at ./stdlib/abort.c:79
#5  0x00007ffff782871b in __assert_fail_base (fmt=0x7ffff79dd130 "%s%s%s:%u: %s%sAssertion `%s' failed.\n%n", assertion=0x555555556066 "ret == 0", file=0x555555556028 "/mnt/d/vsc_project/TinyWebServer_v1/test/test_utils.h", line=25, function=<optimized out>) at ./assert/assert.c:94
#6  0x00007ffff7839e96 in __GI___assert_fail (assertion=0x555555556066 "ret == 0", file=0x555555556028 "/mnt/d/vsc_project/TinyWebServer_v1/test/test_utils.h", line=25, function=0x555555556008 "int ConnectToServer(int)") at ./assert/assert.c:103
#7  0x00005555555555a7 in ConnectToServer (port=8080) at /mnt/d/vsc_project/TinyWebServer_v1/test/test_utils.h:25
#8  0x00005555555553af in main () at /mnt/d/vsc_project/TinyWebServer_v1/test/test_backpressure.cpp:11
A debugging session is active.

	Inferior 1 [process 4481] will be killed.

Quit anyway? (y or n) [answered Y; input not from terminal]

```
