#pragma once

#include "lobsim/types.hpp"

#include <cstdint>
#include <iostream>
#include <string>

struct FillRecord {
public:
    std::uint64_t seq;
    std::int64_t tsExchange;
    std::int64_t tsReceived;
    std::int64_t priceTicks;
    std::int64_t qtyLots;
    // Passive order
    Side makerSide;
    std::int64_t makerOrderId;
    std::int64_t makerTraderId;
    UpdateSource makerSource;
    // Aggressive order
    Side takerSide;
    std::int64_t takerOrderId;
    std::int64_t takerTraderId;
    UpdateSource takerSource;
    std::string bookKey;
};

struct EventApplyRecord {
    std::uint64_t seq;
    std::int64_t tsExchange;
    std::int64_t tsReceived;

    Side side;
    UpdateType updateType;
    UpdateSource source;

    std::int64_t priceTicks;
    std::int64_t qtyLots;
    std::int64_t orderId;
    std::int64_t traderId;
    std::int64_t aggressorId;
    std::string bookKey;
};

enum class DiagnosticRecordCode : std::uint8_t {
    ADD_DUPLICATE_ORDER_ID = 0,
    DELETE_NON_EXISTING_PAPER_ORDER_ID = 1,
    DELETE_NON_EXISTING_HISTORICAL_ORDER_ID = 2,
    PROVIDED_SIDE_ON_DELETE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID = 3,
    PROVIDED_PRICE_ON_DELETE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID = 4,
    SET_WITH_NEGATIVE_LIQUIDITY_REQUESTED_WAS_SET_TO_ZERO = 5,
    SET_NON_EXISTING_ORDER_ID_IS_REJECTED = 6,
    PROVIDED_SIDE_ON_SET_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID = 7,
    PROVIDED_PRICE_ON_SET_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID = 8,
    REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY = 9,
    REQUESTED_REDUCE_NON_EXISTING_ORDER_ID = 10,
    PROVIDED_SIDE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID = 11,
    PROVIDED_PRICE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID = 12,
    REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID = 13,
    PAPER_ORDER_INVOKES_PASSIVE_MATCH_INSTEAD_OF_AGGRESSIVE_TRADE = 14,
    INVALID_UPDATE_TYPE = 15,
    ADD_INVOKED_WITH_NEGATIVE_QUANTITY = 16,
    REQUESTED_REDUCE_PAPER_ORDER_BY_NEGATIVE_QUANTITY = 17,
    CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK = 18,
    REQUESTED_REDUCE_ORDER_BY_NEGATIVE_QUANTITY = 19,
    SET_LOG_SINK_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR = 20,
    DEPTH_AT_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR = 21,
    L2_TOP_N_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR = 22,
    GET_BEST_PRICE_TICKS_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR = 23,
    HEAP_ENTRY_WITHOUT_BUFFERED_EVENT = 24,
    APPLY_EVENT_REQUESTED_FOR_UNKNOWN_BOOK = 25,
    NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR = 26,
};

enum class DiagnosticRecordSeverity : std::uint8_t { INFO = 0, WARNING = 1, ERROR = 2 };

struct DiagnosticRecord {
    std::uint64_t seq;
    std::int64_t tsExchange;
    std::int64_t tsReceived;
    DiagnosticRecordCode code;
    DiagnosticRecordSeverity severity;
    std::string bookKey;
};

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void onFill(const FillRecord& r) = 0;
    virtual void onEventApply(const EventApplyRecord& r) = 0;
    virtual void onDiagnostic(const DiagnosticRecord& r) = 0;
    virtual void reset() {}
};
