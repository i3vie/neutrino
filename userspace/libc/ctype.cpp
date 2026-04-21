#include "ctype.h"

extern "C" int isspace(int ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' ||
           ch == '\r' || ch == '\f' || ch == '\v';
}
