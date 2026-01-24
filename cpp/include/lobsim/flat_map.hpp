#pragma once

#include <algorithm>
#include <vector>
#include <utility>
#include <functional>

#include <iostream>

namespace lobsim {

template <typename Key, typename Value, typename Compare = std::less<Key>>
class FlatMap {
public:
    using value_type = std::pair<Key, Value>;
    using container_type = std::vector<value_type>;
    using iterator = typename container_type::iterator;
    using const_iterator = typename container_type::const_iterator;
    using reverse_iterator = typename container_type::reverse_iterator;
    using const_reverse_iterator = typename container_type::const_reverse_iterator;

    FlatMap() = default;

    iterator begin() { return data_.begin(); }
    const_iterator begin() const { return data_.begin(); }
    iterator end() { return data_.end(); }
    const_iterator end() const { return data_.end(); }

    reverse_iterator rbegin() { return data_.rbegin(); }
    const_reverse_iterator rbegin() const { return data_.rbegin(); }
    reverse_iterator rend() { return data_.rend(); }
    const_reverse_iterator rend() const { return data_.rend(); }

    bool empty() const { return data_.empty(); }
    std::size_t size() const { return data_.size(); }
    void clear() { data_.clear(); }

    iterator find(const Key& key) {
        auto it = lower_bound(key);
        if (it != end() && !cmp_(key, it->first) && !cmp_(it->first, key)) {
            return it;
        }
        return end();
    }

    const_iterator find(const Key& key) const {
        auto it = lower_bound(key);
        if (it != end() && !cmp_(key, it->first) && !cmp_(it->first, key)) {
            return it;
        }
        return end();
    }

    bool contains(const Key& key) const {
        return find(key) != end();
    }

    std::pair<iterator, bool> emplace(const Key& key, Value&& val) {
        // std::cout << "FlatMap::emplace key=" << key << std::endl;
        auto it = lower_bound(key);
        if (it != end() && !cmp_(key, it->first) && !cmp_(it->first, key)) {
            // std::cout << "  Key exists." << std::endl;
            return {it, false};
        }
        // std::cout << "  Inserting." << std::endl;
        return {data_.insert(it, std::make_pair(key, std::move(val))), true};
    }
    
    // Support copy insertion for compatibility
    std::pair<iterator, bool> emplace(const Key& key, const Value& val) {
        auto it = lower_bound(key);
        if (it != end() && !cmp_(key, it->first) && !cmp_(it->first, key)) {
            return {it, false};
        }
        return {data_.insert(it, std::make_pair(key, val)), true};
    }

    iterator erase(iterator pos) {
        return data_.erase(pos);
    }

    size_t erase(const Key& key) {
        auto it = find(key);
        if (it != end()) {
            data_.erase(it);
            return 1;
        }
        return 0;
    }

    iterator lower_bound(const Key& key) {
        return std::lower_bound(data_.begin(), data_.end(), key, 
            [this](const value_type& pair, const Key& k) { return cmp_(pair.first, k); });
    }
    
    // Overload for lower_bound where key is first argument.
    // Standard lower_bound compares element < value.
    // If Compare is std::less: element < value
    // If Compare is std::greater: element > value
    
    const_iterator lower_bound(const Key& key) const {
        return std::lower_bound(data_.begin(), data_.end(), key, 
            [this](const value_type& pair, const Key& k) { return cmp_(pair.first, k); });
    }

private:
    container_type data_;
    Compare cmp_;
};

} // namespace lobsim
