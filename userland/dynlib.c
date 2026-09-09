// userland/dynlib.c -- a deliberately trivial shared object, existing
// only to be dlopen'd. Arithmetic rather than a constant, so a symbol
// resolved to the wrong function cannot pass the test by luck.
int neoos_dl_answer(int x) { return x * 2 + 1; }
