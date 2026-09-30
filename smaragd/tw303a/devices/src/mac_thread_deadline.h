#pragma once

// Ask macOS to stop coalescing this thread's timers (QBX-124).
//
// PRIVATE to tw_devices and living in devices/src for the same reason the VST3
// editor header does: it names Mach types, and nothing outside this library has
// any business with them.
//
// THE PROBLEM. macOS coalesces timers by a PROPORTION of the requested sleep
// rather than by a fixed quantum, so a thread that sleeps to a deadline wakes
// progressively later the longer it sleeps. Measured on an Apple-silicon box,
// worst overshoot of a plain `std::condition_variable::wait_until` over 20
// repetitions at each duration:
//
//   requested              1 ms     5 ms    10 ms
//   default               0.272    1.337    2.544   ms late
//   QOS_USER_INTERACTIVE  0.280    1.289    3.896   ms late
//   TIME_CONSTRAINT       0.041    0.061    0.028   ms late
//
// TWO THINGS THAT DO NOT WORK, recorded so they are not tried again:
//
//   - Raising the QoS class. The middle row IS the measurement; USER_INTERACTIVE
//     is no better than the default and was worse at 10 ms.
//   - Swapping the primitive for `mach_wait_until()`, which is the obvious
//     candidate because it is the low-level one. Measured at 0.273 / 1.287 /
//     2.555 ms, i.e. indistinguishable from the condition variable: the
//     coalescing is applied to the THREAD, not to the call.
//
// WHY THIS SHAPE. A time-constraint thread is what CoreAudio gives its own
// render thread, and it fits any thread that sleeps until a deadline, does a
// short burst of work, and sleeps again. Crucially it does NOT change how the
// thread waits, so a caller's existing wakeup machinery — MidiOutScheduler's
// `wakeSeq_` no-lost-wakeup dance, for one — is untouched. A `mach_wait_until`
// hybrid would have had to give that up.
//
// It is the macOS sibling of the Windows promotions already in this library:
// `AvSetMmThreadCharacteristicsW( L"Pro Audio", … )` in `file_input.cc` and
// `wasapi_backend.cc`, and the high-resolution waitable timer in
// `midi_out_scheduler.cc`. Failure is ignored everywhere for the same reason it
// is ignored there: the pacing is then as coarse as it was before, never wrong,
// and a sandbox that refuses the policy must not cost the user their audio.

#if defined( __APPLE__ )

#include "tw/core/twlog.h"

#include <mach/mach_init.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>

#include <cstdint>

namespace audio {

// `periodNs` is the caller's nominal wake interval, `computationNs` the work it
// expects to do each time. Overrunning `computation` costs a temporary demotion
// to ordinary scheduling — which is exactly the behaviour this call replaces, so
// a bad estimate degrades rather than breaks. Keep the budget modest and honest.
inline void twMacRequestDeadlineScheduling( const char *who,
                                            double periodNs,
                                            double computationNs,
                                            double constraintNs )
{
    mach_timebase_info_data_t tb{};
    if( mach_timebase_info( &tb ) != KERN_SUCCESS || tb.numer == 0 ) return;
    const double nsToTicks = (double) tb.denom / (double) tb.numer;

    thread_time_constraint_policy_data_t p;
    p.period      = (uint32_t) ( periodNs      * nsToTicks );
    p.computation = (uint32_t) ( computationNs * nsToTicks );
    p.constraint  = (uint32_t) ( constraintNs  * nsToTicks );
    p.preemptible = 1;   // not hard real time; we are pacing, not rendering

    const kern_return_t r =
        thread_policy_set( pthread_mach_thread_np( pthread_self() ),
                           THREAD_TIME_CONSTRAINT_POLICY,
                           (thread_policy_t) &p,
                           THREAD_TIME_CONSTRAINT_POLICY_COUNT );
    if( r != KERN_SUCCESS )
        TW_LOGW( "devices", "%s: the kernel refused time-constraint scheduling "
                            "(%d); pacing will be coarser", who, (int) r );
}

}  // namespace audio

#endif  // __APPLE__
