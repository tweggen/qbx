#ifndef _TWSYSLOG_H_
#define _TWSYSLOG_H_

// syslog() compatibility shim, forwarding into the TwLog sink (proposal 24).
//
// Historically this was a Windows-only shim around vfprintf(stderr) while POSIX
// got the real <syslog.h>. That split was the bug: on Linux and macOS these
// messages went to the system journal, so half the engine's diagnostics were
// invisible to the developer running the app. Every platform now routes to
// TwLog, which owns the console tee, the rotating file, and the in-app dock.
//
// The 36 existing syslog(LOG_*, …) call sites are unchanged — this header is
// the whole bridge. New code should prefer the TW_LOG* macros in twlog.h, which
// carry a module category instead of bucketing everything under "syslog".

#include "tw/core/twlog.h"

#include <stdarg.h>

// Deliberately NOT #include <syslog.h> on POSIX: we define these ourselves so
// the shim behaves identically everywhere. Guarded in case a system header has
// already pulled the real definitions in.
#ifndef LOG_EMERG
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7
#endif

inline ::tw::LogLevel twSyslogLevel( int priority )
{
    if( priority <= LOG_ERR )     return ::tw::LogLevel::Error;   // EMERG..ERR
    if( priority == LOG_WARNING ) return ::tw::LogLevel::Warn;
    if( priority <= LOG_INFO )    return ::tw::LogLevel::Info;    // NOTICE, INFO
    return ::tw::LogLevel::Debug;
}

inline void twSyslogForward( int priority, const char *fmt, ... )
    TW_PRINTF_FMT( 2, 3 );

inline void twSyslogForward( int priority, const char *fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    ::tw::TwLog::instance().vlogf( twSyslogLevel( priority ), "syslog",
                                   nullptr, 0, fmt, ap );
    va_end( ap );
}

// A macro rather than a function named `syslog`, so that a translation unit
// which also sees the platform's own syslog() declaration does not hit a
// static-vs-extern redefinition.
#ifdef syslog
#undef syslog
#endif
#define syslog( ... ) twSyslogForward( __VA_ARGS__ )

#endif
