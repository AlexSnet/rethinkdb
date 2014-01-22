// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include "buffer_cache/alt/alt.hpp"
#include "serializer/config.hpp"

namespace unittest {

TEST(SizeofTest, Sizes) {
    EXPECT_GT(1000u, sizeof(standard_serializer_t));
    EXPECT_GT(1000u, sizeof(alt_cache_t));
    EXPECT_GT(1000u, sizeof(buf_lock_t));
    EXPECT_GT(1000u, sizeof(alt_txn_t));
}

TEST(SizeofTest, SerBuffer) {
    // These values depend on what sizeof(block_id_t) is.
    EXPECT_EQ(16u, sizeof(ls_buf_data_t));
    EXPECT_EQ(16u, sizeof(ser_buffer_t));
    EXPECT_EQ(16u, offsetof(ser_buffer_t, cache_data));
}



}  // namespace unittest
