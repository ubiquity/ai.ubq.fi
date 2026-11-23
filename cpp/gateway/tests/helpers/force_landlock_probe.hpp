#pragma once

#if defined(__linux__) && defined(__aarch64__)
#include <array>
#include <cstddef>
#include <cstdint>

#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace uos::tests {
  // Test-only hook. Force the ABI query in this child without altering the
  // gateway's seccomp program or providing a production override.
  inline bool forceLandlockProbeError(int error) {
    std::array<sock_filter, 6> instructions{{
        {BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(seccomp_data, nr)},
        {BPF_JMP | BPF_JEQ | BPF_K, 0, 3, __NR_landlock_create_ruleset},
        {BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(seccomp_data, args[2])},
        {BPF_JMP | BPF_JEQ | BPF_K, 0, 1, LANDLOCK_CREATE_RULESET_VERSION},
        {BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ERRNO | static_cast<std::uint32_t>(error)},
        {BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW},
    }};
    sock_fprog program{static_cast<unsigned short>(instructions.size()), instructions.data()};
    return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) == 0;
  }
}
#endif
