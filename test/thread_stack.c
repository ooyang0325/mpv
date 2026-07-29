/* mp_thread_create() must hand out a stack deep enough for decoders and
 * dlopen'd libraries with large frames. Apple's 512 KB default is not; a
 * single Rust/C++ frame of a few hundred KB overflows it and the process
 * dies with SIGBUS on the guard page, while the same code runs on Linux
 * and Windows where the default is 8 MB.
 *
 * The probe below both reports the configured size and actually touches a
 * large frame, so this fails if the attribute is dropped *and* if the
 * platform ever silently ignores it. */

#include <stdio.h>
#include <string.h>

#include "osdep/threads.h"
#include "test_utils.h"

// Comfortably larger than any plausible default, smaller than what we ask for.
#define PROBE_BYTES (1024 * 1024)

static MP_THREAD_VOID probe(void *arg)
{
    // Defeat the optimizer: the array must really live on the stack.
    volatile char big[PROBE_BYTES];
    memset((void *)big, 0x5a, sizeof(big));
    ((volatile char *)big)[0] = 1;
    ((volatile char *)big)[PROBE_BYTES - 1] = 1;
    *(int *)arg = big[0] + big[PROBE_BYTES - 1];
    MP_THREAD_RETURN();
}

int main(void)
{
#ifdef MP_THREAD_STACK_SIZE
    // Documented expectation, not just whatever the platform happens to do.
    assert_int_equal(MP_THREAD_STACK_SIZE, 8 * 1024 * 1024);
    assert_true(MP_THREAD_STACK_SIZE > PROBE_BYTES);
#elif defined(__APPLE__)
    // Apple's default is 512 KB, which PROBE_BYTES alone would overflow.
    assert_true(!"mp_thread_create() must set an explicit stack size on Apple");
#endif

    int touched = 0;
    mp_thread t;
    assert_int_equal(mp_thread_create(&t, probe, &touched), 0);
    mp_thread_join(t);
    // If the stack were too small the thread would have died on the guard
    // page instead of reaching this.
    assert_int_equal(touched, 2);

    return 0;
}
