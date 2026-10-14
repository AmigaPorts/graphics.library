/* SPDX-License-Identifier: MPL-2.0 */
// Compiler arithmetic support for CPUs without long multiply/divide instructions.
#include <stdint.h>

static uint32_t multiplyWord(uint16_t left, uint16_t right)
{
    uint32_t product = left;
    // Keep this primitive explicit: widening a C multiply may make the
    // compiler call __mulsi3 again, even after narrowing both operands.
    __asm__("mulu.w %1,%0" : "+d"(product) : "d"(right) : "cc");

    return product;
}

uint32_t __mulsi3(uint32_t left, uint32_t right)
{
    uint32_t low = multiplyWord(left, right);
    uint32_t cross = multiplyWord(left >> 16, right);
    cross += multiplyWord(left, right >> 16);

    return low + (cross << 16);
}

uint64_t __muldi3(uint64_t left, uint64_t right)
{
    uint32_t left_low = left, left_high = left >> 32;
    uint32_t right_low = right, right_high = right >> 32;
    uint16_t a[4] = {left_low, left_low >> 16, left_high, left_high >> 16};
    uint16_t b[4] = {right_low, right_low >> 16, right_high, right_high >> 16};
    uint16_t result[4] = {0};

    // Each partial product plus the previous digit and carry fits in 32 bits.
    // Products beyond the fourth digit cannot affect the low 64-bit result.
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t carry = 0;

        for (unsigned j = 0; j < 4 - i; ++j) {
            uint32_t product = multiplyWord(a[i], b[j]) + result[i + j] + carry;
            result[i + j] = product;
            carry = product >> 16;
        }
    }

    uint32_t low = ((uint32_t)result[1] << 16) | result[0];
    uint32_t high = ((uint32_t)result[3] << 16) | result[2];

    return ((uint64_t)high << 32) | low;
}

static uint32_t divide(uint32_t numerator, uint32_t denominator, uint32_t* remainder)
{
    uint32_t quotient = 0, rest = 0;

    if (!denominator) {
        // Preserve the CPU's integer divide-by-zero exception.
        __asm__ volatile("divu.w %1,%0" : "+d"(numerator) : "d"((uint16_t)0) : "cc");
        *remainder = numerator;

        return numerator;
    }

    for (unsigned bit = 0; bit < 32; ++bit) {
        uint32_t high = rest >> 31;
        rest = (rest << 1) | (numerator >> 31);
        numerator <<= 1;
        quotient <<= 1;

        if (high || rest >= denominator) {
            rest -= denominator;
            quotient |= 1;
        }
    }

    *remainder = rest;

    return quotient;
}

uint32_t __udivsi3(uint32_t numerator, uint32_t denominator)
{
    uint32_t remainder;

    return divide(numerator, denominator, &remainder);
}

uint32_t __umodsi3(uint32_t numerator, uint32_t denominator)
{
    uint32_t remainder;
    divide(numerator, denominator, &remainder);

    return remainder;
}

int32_t __divsi3(int32_t numerator, int32_t denominator)
{
    uint32_t left = numerator < 0 ? 0U - (uint32_t)numerator : (uint32_t)numerator;
    uint32_t right = denominator < 0 ? 0U - (uint32_t)denominator : (uint32_t)denominator;
    uint32_t remainder;
    uint32_t quotient = divide(left, right, &remainder);

    return (numerator < 0) != (denominator < 0) ? 0U - quotient : quotient;
}
