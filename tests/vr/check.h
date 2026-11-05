#pragma once
// Shared by the engine-free unit tests: a failed check prints its line and
// expression and exits with 1.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static inline bool Near(float a, float b, float e=1e-4f) { return std::fabs(a-b)<e; }
