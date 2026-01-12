#pragma once

#include <concepts>
namespace lobsim::replay {
template <typename Source, typename RawEvent>
concept IEventSource = requires(Source s, RawEvent& out) {
    { s.next(out) } -> std::same_as<bool>;
};

template <typename Source>
concept IResettableEventSource = requires(Source s) {
    { s.reset() } -> std::same_as<void>;
};

} // namespace lobsim::replay