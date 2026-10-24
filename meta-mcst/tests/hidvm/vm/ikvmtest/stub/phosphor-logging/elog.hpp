#pragma once
// stand-in for phosphor-logging: messages to stderr
#include <cstdio>
namespace phosphor { namespace logging {
enum class level { EMERG, ALERT, CRIT, ERR, WARNING, NOTICE, INFO, DEBUG };
inline const char* entry(const char*, ...) { return ""; }
template <level L, class... A> void log(const char* msg, A...) { fprintf(stderr, "log: %s\n", msg); }
template <class T, class... A> void elog(A...) { throw T(); }
}}
