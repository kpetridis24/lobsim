#pragma once

#include <cstdint>
#include <iostream>

enum class Side : std::uint8_t { SELL = 0, BUY = 1 };

enum class UpdateType : std::uint8_t { ADD = 0, DELETE = 1, SUBTRACT = 2, MATCH = 3, SET = 4, AGGRESSIVE_TRADE = 5 };

enum class UpdateSource : std::uint8_t { HISTORICAL = 0, STRATEGY = 1, UNKNOWN = 2 };

enum class LatencyModelType : std::uint8_t { FIXED = 0, UNIFORM = 1, LOGNORMAL = 2 };

struct LatencyModel {
    LatencyModelType type{LatencyModelType::FIXED};
    double param1{0.0}; // Fixed: value (us), Uniform: min (us), LogNormal: mu (log-scale)
    double param2{0.0}; // Uniform: max (us), LogNormal: sigma (log-scale)
};
