#include <gtest/gtest.h>

#include <cmath>
#include <limits>

// The build's floating-point guarantees (D-008): IEEE 754 types, and no
// fast-math, which would let the compiler assume NaN never occurs.
TEST(Toolchain, FloatingPointIsIeeeWithoutFastMath) {
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    FAIL() << "built with fast-math";
#endif
    EXPECT_TRUE(std::numeric_limits<double>::is_iec559);
    EXPECT_TRUE(std::numeric_limits<float>::is_iec559);

    volatile double zero = 0.0;
    const double nan = zero / zero;
    EXPECT_TRUE(std::isnan(nan));
    EXPECT_NE(nan, nan);
}
