// Darling bug workaround: a process that calls setsid() and then posix_spawn() dies with SIGSEGV.
//
// Crashpad's handler spawner does exactly that (util/posix/spawn_subprocess.cc: fork, setsid,
// posix_spawn, _exit). The intermediate process is killed before its _exit, so Roblox's waitpid
// reports "intermediate process terminated by signal 11", the handshake pipe returns EOF, and the
// client throws "Failed to initialize crash reporter" and exits 1.
//
// Proved with probes/spawn.c, which bisects the three flags independently:
//   fork=1 setsid=0 cloexec=*  -> fine          fork=1 setsid=1 cloexec=* -> SIGSEGV
//   fork=0 setsid=1 (no spawn) -> fine          fork=0 setsid=1 + spawn   -> caller exits 139
// The spawned binary is irrelevant: /usr/bin/sw_vers reproduces it as readily as RobloxCrashHandler.
//
// A new session buys the crash handler nothing here (no controlling terminal to detach from, and
// the guest session dies with the client anyway), so the safe fix is to not create one. Returning
// the current process-group id is what setsid() would have returned had the caller already been a
// group leader; Crashpad ignores the value.
//
// ponytail: the real fix belongs in darlingserver's session bookkeeping. Revisit if a Darling
// update lands; the probe above is the regression test.
extern int getpgrp(void);
int setsid(void) { return getpgrp(); }
