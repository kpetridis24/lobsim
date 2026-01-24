#pragma once

#include "lobsim/lob_event.hpp"
#include "types.hpp"

#include <cstdint>
#include <iostream>
#include <list>
#include <optional>
#include <queue>
#include <span>
#include <tuple>
#include <unordered_map>
#include <vector>

class IMatchingEngine {
public:
    IMatchingEngine() = default;
    virtual ~IMatchingEngine() = default;

    virtual void update(const NormalizedLobEvent& event) = 0;

    virtual void init_from_l2_snapshot(std::span<const Side> sides, std::span<const std::int64_t> prices,
                                       std::span<const std::int64_t> quantities) = 0;

    virtual void init_from_l3_snapshot(std::span<const Side> sides, std::span<const std::int64_t> prices,
                                       std::span<const std::int64_t> quantities,
                                       std::span<const std::int64_t> order_ids,
                                       std::span<const std::int64_t> trader_ids) = 0;
};
