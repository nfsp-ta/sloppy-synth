/* Prints where a test crashed, so CI logs show more than "SegFault".
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
#pragma once

#if defined(__linux__) && !defined(__ANDROID__)
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <link.h>
#include <ucontext.h>
#include <unistd.h>

namespace crash_report {
  inline void printAddress(const char* label, void* address) {
    Dl_info info {};
    char line[512];
    if (dladdr(address, &info) && info.dli_fname) {
      uintptr_t offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(info.dli_fbase);
      std::snprintf(line, sizeof(line), "  %s %p %s+0x%lx (%s)\n", label, address, info.dli_fname,
                    static_cast<unsigned long>(offset), info.dli_sname ? info.dli_sname : "?");
    }
    else
      std::snprintf(line, sizeof(line), "  %s %p\n", label, address);
    ssize_t ignored = write(STDERR_FILENO, line, std::strlen(line));
    (void)ignored;
  }

  inline void handler(int signal, siginfo_t* info, void* context) {
    char line[128];
    std::snprintf(line, sizeof(line), "\nfatal signal %d, fault address %p\n", signal, info->si_addr);
    ssize_t ignored = write(STDERR_FILENO, line, std::strlen(line));
    (void)ignored;

    const mcontext_t& m = static_cast<ucontext_t*>(context)->uc_mcontext;
#if defined(__arm__)
    printAddress("pc", reinterpret_cast<void*>(m.arm_pc));
    printAddress("lr", reinterpret_cast<void*>(m.arm_lr));
#elif defined(__aarch64__)
    printAddress("pc", reinterpret_cast<void*>(m.pc));
    printAddress("lr", reinterpret_cast<void*>(m.regs[30]));
#elif defined(__x86_64__)
    printAddress("pc", reinterpret_cast<void*>(m.gregs[REG_RIP]));
#endif
    void* frames[32];
    int count = backtrace(frames, 32);
    for (int i = 0; i < count; ++i)
      printAddress("frame", frames[i]);

    std::signal(signal, SIG_DFL);
    raise(signal);
  }

  inline void install() {
    struct sigaction action {};
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
  }
}
#else
namespace crash_report {
  inline void install() { }
}
#endif
