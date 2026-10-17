#pragma once
#ifdef _WIN32
#include <windows.h>
#include <stdexcept>
inline double processCpuSeconds() {
    FILETIME created,exited,kernel,user;
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))
        throw std::runtime_error("GetProcessTimes failed");
    const auto ticks=[](FILETIME t) {return (std::uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    return double(ticks(kernel)+ticks(user))*1e-7;
}
#else
#include <ctime>
inline double processCpuSeconds() {return double(std::clock())/CLOCKS_PER_SEC;}
#endif
