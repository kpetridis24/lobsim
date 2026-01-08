#pragma once

#include <cstdint>
#include <iostream>

enum class Side
{
    SELL = 0,
    BUY = 1
};

enum class UpdateType
{
    ADD = 0,
    CANCEL = 1,
    DELETE = 2,
    MATCH = 3,
    SET = 4
};

enum class UpdateSource
{
    HISTORICAL = 0,
    STRATEGY = 1,
};
