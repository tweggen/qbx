#ifndef _TWSYSLOG_H_
#define _TWSYSLOG_H_

#if defined(_WIN32) || defined(_WIN64)

#include <stdio.h>
#include <stdarg.h>

#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

static inline void syslog(int priority, const char *fmt, ...)
{
    (void)priority;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

#else

#include <syslog.h>

#endif

#endif
