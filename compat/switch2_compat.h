// Force-included in every translation unit of the Switch 2 build.
// newlib/libounce lacks the glibc LFS names the SDK's POSIX code uses.
#pragma once
#ifdef __cplusplus
#include <cstdio>
#include <ctime>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#define off64_t off_t
#define fseeko64 fseeko
#define ftello64 ftello
#define ftruncate64 ftruncate
#define fopen64 fopen
#define lseek64 lseek
#define mmap64 mmap
#define stat64 stat
#define fstat64 fstat
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC

#ifdef __cplusplus
extern "C++" {
// newlib has no timegm.
static inline time_t timegm(struct tm* t) {
  static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  long y = t->tm_year + 1900L;
  long days = (y - 1970) * 365 + ((y - 1969) / 4) - ((y - 1901) / 100) + ((y - 1601) / 400);
  days += cum[t->tm_mon % 12] + t->tm_mday - 1;
  if (t->tm_mon > 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
  return (time_t)(((days * 24 + t->tm_hour) * 60 + t->tm_min) * 60 + t->tm_sec);
}
}
#endif
