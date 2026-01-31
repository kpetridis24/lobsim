#pragma once

#if defined(_MSC_VER)
#define LOBSIM_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define LOBSIM_FORCEINLINE inline __attribute__((always_inline))
#else
#define LOBSIM_FORCEINLINE inline
#endif
