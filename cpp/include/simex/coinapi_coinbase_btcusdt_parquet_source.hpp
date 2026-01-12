#pragma once

#include "simex/lob_event.hpp"
#include "simex/types.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace simex::replay {

struct CoinapiCoinbaseBTCUSDTParquetSource {
public:
    explicit CoinapiCoinbaseBTCUSDTParquetSource(std::string path, std::int64_t batchSizeRows = 1 << 15);

    bool next(CoinapiCoinbaseBTCUSDTRawEvent& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace simex::replay
