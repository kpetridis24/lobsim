#pragma once

#include "lobsim/lob_event.hpp"
#include "types.hpp"

#include <cstdint>
#include <iostream>
#include <list>
#include <optional>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <vector>

class IMatchingEngine {
public:
    IMatchingEngine() = default;
    virtual ~IMatchingEngine() = default;

    virtual void update(const NormalizedLobEvent& event) = 0;

    virtual void initFromL2Snapshot(const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
                                    const std::vector<std::int64_t>& quantities) = 0;

    virtual void initFromL3Snapshot(const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
                                    const std::vector<std::int64_t>& quantities,
                                    const std::vector<std::int64_t>& orderIds,
                                    const std::vector<std::int64_t>& traderIds) = 0;
};
