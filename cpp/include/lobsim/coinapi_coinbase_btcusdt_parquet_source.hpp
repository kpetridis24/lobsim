#pragma once

#include "lobsim/lob_event.hpp"
#include "lobsim/types.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace lobsim::replay {

struct CoinapiCoinbaseBTCUSDTParquetSource {
public:
    explicit CoinapiCoinbaseBTCUSDTParquetSource(std::string path, std::int64_t batchSizeRows = 1 << 15);
    ~CoinapiCoinbaseBTCUSDTParquetSource();

    bool next(CoinapiCoinbaseBTCUSDTRawEvent& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace lobsim::replay
