#ifndef SM2FIX_CPU_H
#define SM2FIX_CPU_H

/* Makes the game's main module see at most `limit` processors. Returns the number of
 * import slots patched (0 if nothing was changed). */
unsigned int sm2_install_cpu_limit(unsigned int limit);
/* Number of threads currently in this process (to tell whether we loaded early). */
unsigned int sm2_count_process_threads(void);

#endif
