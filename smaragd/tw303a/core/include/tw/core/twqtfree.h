#ifndef _TW_CORE_TWQTFREE_H_
#define _TW_CORE_TWQTFREE_H_

// A Qt-free claim a raw std::thread can MAKE, and a test seam that can CHECK it
// (QBX-305, docs/contracts/THREADING.md rule 1).
//
// Rule 1 says a raw std::thread must never touch QObject machinery: the first
// QObject constructed on such a thread makes Qt ADOPT it (QThreadData::current()
// builds a QAdoptedThread), and the adopted thread's Qt-TLS cleanup then runs at
// DLL THREAD_DETACH when the thread exits. That cleanup deadlocked a join once
// (twSpeaker::stopOutput) and crashed the sampler's generation worker on
// Windows (QBX-305: twSampleSource opened its file with a QFile).
//
// A worker that claims to be Qt-free opens a twQtFreeScope for the work it
// claims it for. Nothing else happens in a product run: the scope is one
// thread_local int. Under the seam, twQtFreeWatch::install() hooks Qt's own
// QObject-construction callback (qtHookData[AddQObject], the hook GammaRay
// uses), and every QObject constructed on a thread while a scope is open is
// counted as a VIOLATION.
//
// Why this witness sees ADOPTION and not only "a QFile": adoption itself
// constructs a QObject -- the QAdoptedThread -- ON the adopting thread, inside
// QThreadData::current(). So whatever route makes Qt adopt the thread (a QFile,
// a QTimer, QThread::currentThread(), a signal's sender lookup), the hook fires
// on that thread. Zero violations means no QObject was born inside the scope,
// adoption included.
//
// The hook chains any callback installed before it, and is installed once for
// the process; reset() zeroes the counters, it never uninstalls.

#include <cstdint>

class twQtFreeScope
{
public:
    twQtFreeScope();
    ~twQtFreeScope();
    twQtFreeScope( const twQtFreeScope & ) = delete;
    twQtFreeScope &operator=( const twQtFreeScope & ) = delete;

    // True while this thread is inside at least one scope.
    static bool active();
};

namespace twQtFreeWatch {

// Install the QObject-construction hook (idempotent). Returns false when this
// Qt does not offer the hook table (then nothing can be watched, and a gate
// built on it must fail rather than pass).
bool install();
bool installed();

// Zero both counters.
void reset();

// QObjects constructed on any thread while that thread was inside a scope.
std::uint64_t violations();

// Scopes OPENED since the last reset(). A gate asserts this too: a worker that
// loses its scope would otherwise report zero violations by never looking.
std::uint64_t scopesEntered();

}  // namespace twQtFreeWatch

#endif
