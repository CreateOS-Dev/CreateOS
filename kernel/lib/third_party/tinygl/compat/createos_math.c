#include "math.h"

static double reduce_angle(double value)
{
    while (value > M_PI)
        value -= 2.0 * M_PI;
    while (value < -M_PI)
        value += 2.0 * M_PI;
    return value;
}

double fabs(double value)
{
    return value < 0.0 ? -value : value;
}

double floor(double value)
{
    int truncated = (int)value;
    return (double)(truncated - (truncated > value));
}

double sqrt(double value)
{
    if (value <= 0.0)
        return 0.0;
    double estimate = value > 1.0 ? value : 1.0;
    for (int i = 0; i < 32; i++)
        estimate = 0.5 * (estimate + value / estimate);
    return estimate;
}

double sin(double value)
{
    value = reduce_angle(value);
    double squared = value * value;
    double term = value;
    double result = term;
    for (int i = 1; i < 9; i++) {
        term *= -squared / ((2.0 * i) * (2.0 * i + 1.0));
        result += term;
    }
    return result;
}

double cos(double value)
{
    return sin(value + M_PI / 2.0);
}

static double logarithm(double value)
{
    int exponent = 0;
    while (value >= 2.0) {
        value *= 0.5;
        exponent++;
    }
    while (value < 1.0) {
        value *= 2.0;
        exponent--;
    }
    double ratio = (value - 1.0) / (value + 1.0);
    double squared = ratio * ratio;
    double term = ratio;
    double result = 0.0;
    for (int i = 1; i < 32; i += 2) {
        result += term / i;
        term *= squared;
    }
    return 2.0 * result + exponent * 0.6931471805599453;
}

static double exponential(double value)
{
    int power_of_two = 0;
    while (value > 0.6931471805599453) {
        value -= 0.6931471805599453;
        power_of_two++;
    }
    while (value < -0.6931471805599453) {
        value += 0.6931471805599453;
        power_of_two--;
    }
    double term = 1.0;
    double result = 1.0;
    for (int i = 1; i < 24; i++) {
        term *= value / i;
        result += term;
    }
    while (power_of_two > 0) {
        result *= 2.0;
        power_of_two--;
    }
    while (power_of_two < 0) {
        result *= 0.5;
        power_of_two++;
    }
    return result;
}

double pow(double base, double exponent)
{
    if (base == 0.0)
        return exponent > 0.0 ? 0.0 : 1.0;
    if (base < 0.0) {
        int integer_exponent = (int)exponent;
        if ((double)integer_exponent != exponent)
            return 0.0;
        double result = exponential(exponent * logarithm(-base));
        return integer_exponent & 1 ? -result : result;
    }
    return exponential(exponent * logarithm(base));
}

__asm__(".section .note.GNU-stack,\"\",@progbits");
