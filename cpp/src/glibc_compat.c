/* PortMaster build: the cross gcc compiles against glibc 2.35 headers, so libstdc++ reads
   __libc_single_threaded (glibc 2.32+). Older device glibc lacks it; 0 keeps every atomic
   operation, and a newer glibc's own copy is simply shadowed. */
char __libc_single_threaded = 0;
