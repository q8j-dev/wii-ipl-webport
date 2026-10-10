// Out-of-line MWCC intrinsics, for code that calls them through an extern
// declaration instead of the inline versions in web/include/decomp/ide.h.

unsigned int __web_cntlzw(unsigned int v) { return v ? (unsigned int)__builtin_clz(v) : 32u; }
int __web_abs(int n) { return n < 0 ? -n : n; }

unsigned int __cntlzw(unsigned int v) __attribute__((alias("__web_cntlzw")));
int __abs(int n) __attribute__((alias("__web_abs")));
