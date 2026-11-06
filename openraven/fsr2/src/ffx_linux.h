// Portability shim so the Windows-only FSR 2 GL backend builds with GCC on Linux (force-included).
#pragma once
#define FFX_GCC 1
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef __cplusplus
template <class T, size_t N> constexpr size_t ffx_countof(T (&)[N]) { return N; }
#define _countof(a) ffx_countof(a)
template <size_t N> static inline int wcscpy_s(wchar_t (&d)[N], const wchar_t *s) { wcsncpy(d, s, N - 1); d[N - 1] = 0; return 0; }
static inline int wcscpy_s(wchar_t *d, size_t n, const wchar_t *s) { wcsncpy(d, s, n - 1); d[n - 1] = 0; return 0; }
static inline int wcstombs_s(size_t *ret, char *d, size_t dsz, const wchar_t *s, size_t) { size_t r = wcstombs(d, s, dsz - 1); d[dsz - 1] = 0; if (ret) *ret = r + 1; return 0; }
#endif
#define sprintf_s snprintf
