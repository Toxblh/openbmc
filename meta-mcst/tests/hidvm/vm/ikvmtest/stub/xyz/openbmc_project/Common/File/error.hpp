#pragma once
namespace sdbusplus { namespace xyz { namespace openbmc_project { namespace Common { namespace File { namespace Error {
struct Open {};
}}}}}}
namespace xyz { namespace openbmc_project { namespace Common { namespace File { namespace Open {
inline int ERRNO(int) { return 0; }
inline int PATH(const char*) { return 0; }
}}}}}
