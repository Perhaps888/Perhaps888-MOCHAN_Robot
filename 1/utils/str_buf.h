#pragma once
#include "../config.h"
#include <cstdarg>
// 预分配 + concat（不 realloc），用于 /scan /get_wifi_mode 等 JSON 组装热点
struct StrBuf : public String {
  StrBuf(size_t capBytes) { reserve(capBytes); }
  StrBuf& catf(const char *fmt, ...) {
    char b[256]; va_list ap; va_start(ap,fmt);
    int n = vsnprintf(b,sizeof(b),fmt,ap);
    va_end(ap);
    if (n>0) concat(b, (unsigned)n);
    return *this;
  }
};
