#pragma once

#include "lobsim/flat_hash_map.hpp"

#include <functional>
#include <utility>

namespace lobsim {

template <typename K, typename V, typename H = std::hash<K>, typename E = std::equal_to<K>,
          typename A = std::allocator<std::pair<K, V>>>
class FlatHashMap : public ska::flat_hash_map<K, V, H, E, A> {
    using Base = ska::flat_hash_map<K, V, H, E, A>;

public:
    using Base::Base;

    bool contains(const K& key) const { return this->find(key) != this->end(); }
};

} // namespace lobsim
