#pragma once

#include "lobsim/lob_event.hpp"
#include "lobsim/types.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace lobsim::replay {

struct CoinbaseBTCUSDTParquetSource {
public:
    explicit CoinbaseBTCUSDTParquetSource(std::string path, std::int64_t batch_size_rows = 1 << 15);
    ~CoinbaseBTCUSDTParquetSource();

    bool next(CoinbaseBTCUSDTRawEvent& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace lobsim::replay
