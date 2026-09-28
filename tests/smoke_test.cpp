#include <gtest/gtest.h>

#include "memloc/memloc.h"

TEST(Smoke, VersionIsReported) { EXPECT_STREQ(memloc_version(), "0.1.0"); }
