#pragma once

// Branch prediction hints for hot paths.
// Safe no-ops on compilers that don't support __builtin_expect.
#if defined(__GNUC__) || defined(__clang__)
#define BRANCH_LIKELY(x) (__builtin_expect(!!(x), 1))
#define BRANCH_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define BRANCH_LIKELY(x) (x)
#define BRANCH_UNLIKELY(x) (x)
#endif
