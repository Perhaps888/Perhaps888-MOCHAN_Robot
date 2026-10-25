#pragma once
#include "../config.h"
static inline String jsonEscape(const String &s) {
  String o; o.reserve(s.length()+4);
  for (unsigned i=0;i<s.length();i++){
    char c=s[i];
    switch(c){
      case '"':  o+="\\\""; break;
      case '\\': o+="\\\\"; break;
      case '\b': o+="\\b";  break;
      case '\f': o+="\\f";  break;
      case '\n': o+="\\n";  break;
      case '\r': o+="\\r";  break;
      case '\t': o+="\\t";  break;
      default:
        if ((unsigned char)c<0x20){ char b[8]; snprintf(b,sizeof(b),"\\u%04x",c); o+=b; }
        else o+=c;
    }
  }
  return o;
}
