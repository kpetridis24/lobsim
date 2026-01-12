#pragma once

#include "simex/lob_event.hpp"

#include <concepts>
#include <iostream>

template <typename Adapter, typename RawEvent>
concept IEventAdapter = requires(Adapter a, const RawEvent& raw) {
    { a.normalize(raw) } -> std::same_as<NormalizedLobEvent>;
};