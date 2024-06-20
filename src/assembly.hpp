#include <cstdint>

#pragma once

namespace assembly {

inline void cpuid() {
    asm volatile(
        "cpuid\n\t" ::: "%rax", "%rbx", "%rcx", "%rdx");
}

#if defined(__riscv)


inline uint64_t rdtsc() {
    uint64_t val;
    asm volatile("rdcycle %0" : "=r"(val));
    return val;
}

inline uint64_t rdtscp() {
    return rdtsc();
}

#elif defined(__x86_64__)

inline uint64_t rdtsc() {
    uint32_t cycles_high, cycles_low;
    asm volatile(
        "rdtsc\n\t"
        "mov %%edx, %0\n\t"
        "mov %%eax, %1\n\t"
        : "=r"(cycles_high), "=r"(cycles_low)
        :
        : "%rax", "%rdx");
    return (uint64_t(cycles_high) << 32) | cycles_low;
}

inline uint64_t rdtscp() {
    uint32_t cycles_high, cycles_low;
    asm volatile(
        "rdtscp\n\t"
        "mov %%edx, %0\n\t"
        "mov %%eax, %1\n\t"
        : "=r"(cycles_high), "=r"(cycles_low)
        :
        : "%rax", "%rcx", "%rdx");
    return (uint64_t(cycles_high) << 32) | cycles_low;
}

#endif

}
