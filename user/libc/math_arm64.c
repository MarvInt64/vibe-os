/* Small libm fallback for aarch64 userspace.
 * Good enough for Browser/QuickJS math without pulling in a full libm. */

#ifdef ARCH_ARM64

static const double PI = 3.14159265358979323846;
static const double LN2 = 0.69314718055994530942;

double fabs(double x) { return x < 0.0 ? -x : x; }

double floor(double x) {
    long i = (long)x;
    return ((double)i > x) ? (double)(i - 1) : (double)i;
}

double ceil(double x) {
    long i = (long)x;
    return ((double)i < x) ? (double)(i + 1) : (double)i;
}

double trunc(double x) { return (double)(long)x; }

double round(double x) { return x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5); }

double fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    return x - trunc(x / y) * y;
}

double sqrt(double x) {
    double r;
    int i;
    if (x <= 0.0) return 0.0;
    r = x >= 1.0 ? x : 1.0;
    for (i = 0; i < 24; ++i) r = 0.5 * (r + x / r);
    return r;
}

static double reduce_pi(double x) {
    x = fmod(x, 2.0 * PI);
    if (x > PI) x -= 2.0 * PI;
    if (x < -PI) x += 2.0 * PI;
    return x;
}

double sin(double x) {
    x = reduce_pi(x);
    double x2 = x * x;
    return x * (1.0 - x2 / 6.0 + (x2 * x2) / 120.0 - (x2 * x2 * x2) / 5040.0);
}

double cos(double x) {
    x = reduce_pi(x);
    double x2 = x * x;
    return 1.0 - x2 / 2.0 + (x2 * x2) / 24.0 - (x2 * x2 * x2) / 720.0;
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return x < 0.0 ? -1.0e300 : 1.0e300;
    return sin(x) / c;
}

double atan(double x) {
    int neg = x < 0.0;
    if (neg) x = -x;
    double r;
    if (x > 1.0) {
        r = PI / 2.0 - atan(1.0 / x);
    } else {
        double x2 = x * x;
        r = x * (1.0 - x2 / 3.0 + x2 * x2 / 5.0 - x2 * x2 * x2 / 7.0);
    }
    return neg ? -r : r;
}

double atan2(double y, double x) {
    if (x > 0.0) return atan(y / x);
    if (x < 0.0 && y >= 0.0) return atan(y / x) + PI;
    if (x < 0.0 && y < 0.0) return atan(y / x) - PI;
    if (y > 0.0) return PI / 2.0;
    if (y < 0.0) return -PI / 2.0;
    return 0.0;
}

double asin(double x) {
    if (x >= 1.0) return PI / 2.0;
    if (x <= -1.0) return -PI / 2.0;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) { return PI / 2.0 - asin(x); }

double exp(double x) {
    int n = 0;
    while (x > LN2) { x -= LN2; ++n; }
    while (x < -LN2) { x += LN2; --n; }
    double term = 1.0, sum = 1.0;
    for (int i = 1; i <= 18; ++i) {
        term *= x / (double)i;
        sum += term;
    }
    while (n > 0) { sum *= 2.0; --n; }
    while (n < 0) { sum *= 0.5; ++n; }
    return sum;
}

double log(double x) {
    if (x <= 0.0) return -1.0e300;
    int k = 0;
    while (x > 1.5) { x *= 0.5; ++k; }
    while (x < 0.75) { x *= 2.0; --k; }
    double z = (x - 1.0) / (x + 1.0);
    double z2 = z * z;
    double term = z;
    double sum = 0.0;
    for (int n = 1; n < 30; n += 2) {
        sum += term / (double)n;
        term *= z2;
    }
    return 2.0 * sum + (double)k * LN2;
}

double log2(double x) { return log(x) / LN2; }
double log10(double x) { return log(x) / 2.30258509299404568402; }
double log1p(double x) { return log(1.0 + x); }
double exp2(double x) { return exp(x * LN2); }
double expm1(double x) { return exp(x) - 1.0; }
double pow(double x, double y) { return x <= 0.0 ? 0.0 : exp(y * log(x)); }

double sinh(double x) { return 0.5 * (exp(x) - exp(-x)); }
double cosh(double x) { return 0.5 * (exp(x) + exp(-x)); }
double tanh(double x) {
    double e = exp(2.0 * x);
    return (e - 1.0) / (e + 1.0);
}
double asinh(double x) { return log(x + sqrt(x * x + 1.0)); }
double acosh(double x) { return x < 1.0 ? 0.0 : log(x + sqrt(x * x - 1.0)); }
double atanh(double x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
double hypot(double x, double y) { return sqrt(x * x + y * y); }
double cbrt(double x) {
    if (x == 0.0) return 0.0;
    double r = x > 0.0 ? x : -x;
    r = pow(r, 1.0 / 3.0);
    return x < 0.0 ? -r : r;
}

float sinf(float x) { return (float)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }
float expf(float x) { return (float)exp((double)x); }
float logf(float x) { return (float)log((double)x); }
float powf(float x, float y) { return (float)pow((double)x, (double)y); }
float sqrtf(float x) { return (float)sqrt((double)x); }

#endif
