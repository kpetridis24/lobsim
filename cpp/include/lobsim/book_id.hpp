#pragma once

#include <string>

struct BookId {
    std::string venue;
    std::string symbol;

    bool operator==(const BookId& other) const = default;
};

inline std::string book_key(const BookId& id) {
    if (id.venue.empty()) {
        return id.symbol;
    }
    return id.venue + ":" + id.symbol;
}
