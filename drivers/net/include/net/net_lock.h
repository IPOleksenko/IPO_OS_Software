#ifndef IPO_NET_LOCK_H
#define IPO_NET_LOCK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Acquire the recursive network stack synchronization lock for SMP safety.
 */
void net_lock_acquire(void);

/**
 * Attempt to acquire the recursive network stack lock without spinning.
 * Returns true if acquired (must be paired with net_lock_release()).
 */
bool net_lock_trylock(void);

/**
 * Release one nesting level of the recursive network stack lock.
 */
void net_lock_release(void);

/**
 * Temporarily relinquish the network lock across a scheduler yield + io_wait()
 * cycle and reacquire the exact recursion depth upon resuming.
 */
void net_lock_yield_sleep(void);

#endif /* IPO_NET_LOCK_H */
