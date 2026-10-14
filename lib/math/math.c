/* Natives of package math (runtime/README.md, "Native functions"): the
 * float functions of the C library. */
#include "seed.h"

#include <math.h>

double sd_lib_math_powFloat(double base, double exp) { return pow(base, exp); }
double sd_lib_math_sqrt(double x) { return sqrt(x); }
double sd_lib_math_floor(double x) { return floor(x); }
double sd_lib_math_ceil(double x) { return ceil(x); }
double sd_lib_math_round(double x) { return round(x); }
double sd_lib_math_trunc(double x) { return trunc(x); }
double sd_lib_math_sin(double x) { return sin(x); }
double sd_lib_math_cos(double x) { return cos(x); }
double sd_lib_math_tan(double x) { return tan(x); }
double sd_lib_math_asin(double x) { return asin(x); }
double sd_lib_math_acos(double x) { return acos(x); }
double sd_lib_math_atan(double x) { return atan(x); }
double sd_lib_math_atan2(double y, double x) { return atan2(y, x); }
double sd_lib_math_exp(double x) { return exp(x); }
double sd_lib_math_log(double x) { return log(x); }
double sd_lib_math_log2(double x) { return log2(x); }
double sd_lib_math_log10(double x) { return log10(x); }
