#pragma once
#include <stdexcept>
#include <string>
#include <sstream>
#include <cmath>

inline void ASSERT_TRUE(bool cond, const std::string& msg = "ASSERT_TRUE failed") {
    if (!cond) throw std::runtime_error(msg);
}
inline void ASSERT_FALSE(bool cond, const std::string& msg = "ASSERT_FALSE failed") {
    if (cond) throw std::runtime_error(msg);
}
template<typename A, typename B>
inline void ASSERT_EQ(const A& a, const B& b, const std::string& msg = "ASSERT_EQ failed") {
    if (!(a == b)) {
        std::ostringstream oss;
        oss << msg << " (" << a << " != " << b << ")";
        throw std::runtime_error(oss.str());
    }
}
inline void ASSERT_NEAR(double a, double b, double eps = 1e-9,
                         const std::string& msg = "ASSERT_NEAR failed") {
    if (std::fabs(a - b) > eps) {
        std::ostringstream oss;
        oss << msg << " (" << a << " vs " << b << ", tol=" << eps << ")";
        throw std::runtime_error(oss.str());
    }
}
