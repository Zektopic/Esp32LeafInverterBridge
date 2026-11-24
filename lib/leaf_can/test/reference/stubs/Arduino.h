/* Minimal host stub of Arduino.h - just enough to compile the v1.3.0 reference handler. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

typedef bool boolean;
typedef uint8_t byte;

class String {
public:
    String() {}
    String(const char *s) : s_(s ? s : "") {}
    const char *c_str() const { return s_.c_str(); }
private:
    std::string s_;
};
