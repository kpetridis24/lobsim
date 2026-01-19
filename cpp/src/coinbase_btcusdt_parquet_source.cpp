#include "lobsim/coinbase_btcusdt_parquet_source.hpp"

#include "lobsim/types.hpp"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/result.h>
#include <cctype>
#include <memory>
#include <parquet/arrow/reader.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lobsim::replay {

static UpdateType parseUpdateType(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);

    std::string upper;
    upper.reserve(s.size());
    for (char c : s) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    if (upper == "SNAPSHOT")
        return UpdateType::SET;
    if (upper == "ADD")
        return UpdateType::ADD;
    if (upper == "DELETE")
        return UpdateType::DELETE;
    if (upper == "MATCH")
        return UpdateType::MATCH;
    if (upper == "SUBTRACT")
        return UpdateType::SUBTRACT;
    if (upper == "SET")
        return UpdateType::SET;
    throw std::runtime_error("Unknown update_type: " + std::string(s));
}

static std::int64_t parseHhMmSsToUs(std::string_view sv) {
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };

    // trim
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
        sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
        sv.remove_suffix(1);

    auto read2 = [&](std::size_t off) -> int {
        if (off + 2 > sv.size() || !is_digit(sv[off]) || !is_digit(sv[off + 1]))
            return -1;
        return (sv[off] - '0') * 10 + (sv[off + 1] - '0');
    };

    if (sv.size() < 8 || sv[2] != ':' || sv[5] != ':')
        throw std::runtime_error("Bad time: " + std::string(sv));
    int hh = read2(0);
    int mm = read2(3);
    int ss = read2(6);
    if (hh < 0 || mm < 0 || ss < 0)
        throw std::runtime_error("Bad time: " + std::string(sv));
    if (hh > 23 || mm > 59 || ss > 59)
        throw std::runtime_error("Bad time: " + std::string(sv));

    std::int64_t us = static_cast<std::int64_t>(hh) * 3600'000'000LL + static_cast<std::int64_t>(mm) * 60'000'000LL +
                      static_cast<std::int64_t>(ss) * 1'000'000LL;

    if (sv.size() == 8)
        return us;

    if (sv[8] != '.')
        throw std::runtime_error("Bad time: " + std::string(sv));
    std::size_t i = 9;
    int digits = 0;
    std::int64_t frac = 0;
    while (i < sv.size() && is_digit(sv[i]) && digits < 6) {
        frac = frac * 10 + (sv[i] - '0');
        ++digits;
        ++i;
    }
    while (digits < 6) {
        frac *= 10;
        ++digits;
    }
    return us + frac;
}

struct CoinbaseBTCUSDTParquetSource::Impl {
    std::unique_ptr<parquet::arrow::FileReader> reader;
    std::shared_ptr<arrow::RecordBatchReader> rb_reader;
    std::shared_ptr<arrow::RecordBatch> batch;
    std::int64_t rowInBatch = 0;
    std::int64_t globalRow = 0;

    int col_time_exchange = -1;
    int col_time_feed = -1;
    int col_update_type = -1;
    int col_is_buy = -1;
    int col_entry_px = -1;
    int col_entry_sx = -1;
    int col_order_id = -1;

    bool time_exchange_is_string = true;
    bool time_feed_is_string = true;
    arrow::TimeUnit::type time_exchange_unit{};
    arrow::TimeUnit::type time_feed_unit{};

    explicit Impl(std::string path, std::int64_t batchSizeRows) {
        auto infile_res = arrow::io::ReadableFile::Open(path);
        if (!infile_res.ok()) {
            throw std::runtime_error(infile_res.status().ToString());
        }
        auto infile = *infile_res;

        parquet::arrow::FileReaderBuilder builder;
        builder.memory_pool(arrow::default_memory_pool());
        auto st = builder.Open(infile);
        if (!st.ok()) {
            throw std::runtime_error(st.ToString());
        }
        std::unique_ptr<parquet::arrow::FileReader> reader_local;
        st = builder.Build(&reader_local);
        if (!st.ok()) {
            throw std::runtime_error(st.ToString());
        }
        reader = std::move(reader_local);
        reader->set_batch_size(batchSizeRows);

        std::shared_ptr<arrow::Schema> schema;
        PARQUET_THROW_NOT_OK(reader->GetSchema(&schema));

        auto idx = [&](const char* name) -> int {
            int i = schema->GetFieldIndex(name);
            if (i < 0)
                throw std::runtime_error(std::string("Missing required column: ") + name);
            return i;
        };

        col_time_exchange = idx("time_exchange");
        auto find_time_feed = [&]() -> int {
            int i = schema->GetFieldIndex("time_received");
            if (i < 0)
                i = schema->GetFieldIndex("time_feed");
            if (i < 0) {
                for (int k = 0; k < schema->num_fields(); ++k) {
                    auto name = schema->field(k)->name();
                    if (name.find("time") != std::string::npos && name.find("exchange") == std::string::npos) {
                        i = k;
                        break;
                    }
                }
            }
            if (i < 0)
                throw std::runtime_error("Missing required received time column.");
            return i;
        };
        col_time_feed = find_time_feed();
        col_update_type = idx("update_type");
        col_is_buy = idx("is_buy");
        col_entry_px = idx("entry_px");
        col_entry_sx = idx("entry_sx");
        col_order_id = idx("order_id");

        // Detect time column physical types
        auto t0 = schema->field(col_time_exchange)->type();
        auto t1 = schema->field(col_time_feed)->type();

        time_exchange_is_string = (t0->id() == arrow::Type::STRING) || (t0->id() == arrow::Type::LARGE_STRING);
        time_feed_is_string = (t1->id() == arrow::Type::STRING) || (t1->id() == arrow::Type::LARGE_STRING);

        if (!time_exchange_is_string) {
            if (t0->id() == arrow::Type::TIME64) {
                time_exchange_unit = std::static_pointer_cast<arrow::Time64Type>(t0)->unit();
            } else if (t0->id() == arrow::Type::INT64) {
                // assume already microseconds since midnight
            } else {
                throw std::runtime_error("Unsupported time_exchange type.");
            }
        }
        if (!time_feed_is_string) {
            if (t1->id() == arrow::Type::TIME64) {
                time_feed_unit = std::static_pointer_cast<arrow::Time64Type>(t1)->unit();
            } else if (t1->id() == arrow::Type::INT64) {
                // assume already microseconds since midnight
            } else {
                throw std::runtime_error("Unsupported received time type.");
            }
        }

        // Read all row groups and only needed columns
        std::vector<int> row_groups(reader->num_row_groups());
        for (int i = 0; i < static_cast<int>(row_groups.size()); ++i)
            row_groups[i] = i;

        std::vector<int> cols{col_time_exchange, col_time_feed, col_update_type, col_is_buy,
                              col_entry_px,      col_entry_sx,  col_order_id};

        std::shared_ptr<arrow::RecordBatchReader> rb_reader_local;
        st = reader->GetRecordBatchReader(row_groups, cols, &rb_reader_local);
        if (!st.ok()) {
            throw std::runtime_error(st.ToString());
        }
        rb_reader = std::move(rb_reader_local);
    }

    bool loadNextBatch() {
        batch.reset();
        rowInBatch = 0;
        auto st = rb_reader->ReadNext(&batch);
        if (!st.ok()) {
            throw std::runtime_error(st.ToString());
        }
        return static_cast<bool>(batch);
    }

    std::int64_t readTimeUs(const std::shared_ptr<arrow::Array>& arr, std::int64_t row, bool isString,
                            arrow::TimeUnit::type unit) {
        if (arr->IsNull(row))
            throw std::runtime_error("Null time at row " + std::to_string(globalRow));

        if (isString) {
            if (arr->type_id() == arrow::Type::STRING) {
                auto a = std::static_pointer_cast<arrow::StringArray>(arr);
                return parseHhMmSsToUs(a->GetView(row));
            } else {
                auto a = std::static_pointer_cast<arrow::LargeStringArray>(arr);
                return parseHhMmSsToUs(a->GetView(row));
            }
        }

        if (arr->type_id() == arrow::Type::TIME64) {
            auto a = std::static_pointer_cast<arrow::Time64Array>(arr);
            const std::int64_t v = a->Value(row);
            if (unit == arrow::TimeUnit::MICRO)
                return v;
            if (unit == arrow::TimeUnit::NANO)
                return v / 1000;
            throw std::runtime_error("Unsupported Time64 unit.");
        }

        if (arr->type_id() == arrow::Type::INT64) {
            auto a = std::static_pointer_cast<arrow::Int64Array>(arr);
            return a->Value(row);
        }

        throw std::runtime_error("Unsupported time array type.");
    }

    bool next(CoinbaseBTCUSDTRawEvent& out) {
        while (true) {
            if (!batch || rowInBatch >= batch->num_rows()) {
                if (!loadNextBatch())
                    return false;
            }

            const std::int64_t r = rowInBatch++;

            // Column order in reader matches cols vector order, not original schema indices
            // We requested 7 columns in that exact order:
            // [time_exchange, time_feed, update_type, is_buy, entry_px, entry_sx, order_id]
            auto a_time_exchange = batch->column(0);
            auto a_time_feed = batch->column(1);
            auto a_update_type = batch->column(2);
            auto a_is_buy = batch->column(3);
            auto a_entry_px = batch->column(4);
            auto a_entry_sx = batch->column(5);
            auto a_order_id = batch->column(6);

            out.tsExchangeUs = readTimeUs(a_time_exchange, r, time_exchange_is_string, time_exchange_unit);
            out.tsReceivedUs = readTimeUs(a_time_feed, r, time_feed_is_string, time_feed_unit);

            if (a_update_type->IsNull(r))
                throw std::runtime_error("Null update_type at row " + std::to_string(globalRow));
            std::string_view ut;
            if (a_update_type->type_id() == arrow::Type::STRING) {
                ut = std::static_pointer_cast<arrow::StringArray>(a_update_type)->GetView(r);
            } else {
                ut = std::static_pointer_cast<arrow::LargeStringArray>(a_update_type)->GetView(r);
            }
            out.updateType = parseUpdateType(ut);

            if (a_is_buy->IsNull(r))
                throw std::runtime_error("Null is_buy at row " + std::to_string(globalRow));
            // support int/bool
            if (a_is_buy->type_id() == arrow::Type::INT64) {
                auto v = std::static_pointer_cast<arrow::Int64Array>(a_is_buy)->Value(r);
                out.side = (v == 1) ? Side::BUY : Side::SELL;
            } else if (a_is_buy->type_id() == arrow::Type::INT32) {
                auto v = std::static_pointer_cast<arrow::Int32Array>(a_is_buy)->Value(r);
                out.side = (v == 1) ? Side::BUY : Side::SELL;
            } else if (a_is_buy->type_id() == arrow::Type::BOOL) {
                auto v = std::static_pointer_cast<arrow::BooleanArray>(a_is_buy)->Value(r);
                out.side = v ? Side::BUY : Side::SELL;
            } else {
                throw std::runtime_error("Unsupported is_buy type.");
            }

            if (a_entry_px->IsNull(r) || a_entry_sx->IsNull(r))
                throw std::runtime_error("Null price/size at row " + std::to_string(globalRow));

            if (a_entry_px->type_id() == arrow::Type::DOUBLE) {
                out.price =
                    static_cast<long double>(std::static_pointer_cast<arrow::DoubleArray>(a_entry_px)->Value(r));
            } else if (a_entry_px->type_id() == arrow::Type::FLOAT) {
                out.price = static_cast<long double>(std::static_pointer_cast<arrow::FloatArray>(a_entry_px)->Value(r));
            } else {
                throw std::runtime_error("Unsupported entry_px type.");
            }

            if (a_entry_sx->type_id() == arrow::Type::DOUBLE) {
                out.size = static_cast<long double>(std::static_pointer_cast<arrow::DoubleArray>(a_entry_sx)->Value(r));
            } else if (a_entry_sx->type_id() == arrow::Type::FLOAT) {
                out.size = static_cast<long double>(std::static_pointer_cast<arrow::FloatArray>(a_entry_sx)->Value(r));
            } else {
                throw std::runtime_error("Unsupported entry_sx type.");
            }

            if (a_order_id->IsNull(r))
                throw std::runtime_error("Null order_id at row " + std::to_string(globalRow));
            if (a_order_id->type_id() == arrow::Type::STRING) {
                out.orderId = std::string(std::static_pointer_cast<arrow::StringArray>(a_order_id)->GetView(r));
            } else {
                out.orderId = std::string(std::static_pointer_cast<arrow::LargeStringArray>(a_order_id)->GetView(r));
            }

            ++globalRow;
            return true;
        }
    }
};

CoinbaseBTCUSDTParquetSource::CoinbaseBTCUSDTParquetSource(std::string path, std::int64_t batchSizeRows)
    : impl(std::make_unique<Impl>(std::move(path), batchSizeRows)) {}

CoinbaseBTCUSDTParquetSource::~CoinbaseBTCUSDTParquetSource() = default;

bool CoinbaseBTCUSDTParquetSource::next(CoinbaseBTCUSDTRawEvent& out) {
    return impl->next(out);
}

} // namespace lobsim::replay
