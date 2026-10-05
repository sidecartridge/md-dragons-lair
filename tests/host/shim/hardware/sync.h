/* Host stand-in for the Pico SDK's hardware/sync.h. */
#ifndef HOST_SHIM_HARDWARE_SYNC_H
#define HOST_SHIM_HARDWARE_SYNC_H
#define __dmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#include <stdint.h>
/* The tests call the writers themselves: nothing to mask. */
static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t status) { (void)status; }
#endif
