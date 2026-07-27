#include "pch.h"
#include "gtest/gtest.h"
#include "StringUtils.h"

TEST(StringUtilsTest, SplitAndTrimCSV) {
    auto res1 = QuickView::SplitAndTrimCSV(L"A, B, , C, A");
    ASSERT_EQ(res1.size(), 4);
    EXPECT_EQ(res1[0], L"A");
    EXPECT_EQ(res1[1], L"B");
    EXPECT_EQ(res1[2], L"C");
    EXPECT_EQ(res1[3], L"A");
}

TEST(StringUtilsTest, NormalizeCSV) {
    std::vector<std::wstring> allowed = { L"A", L"B", L"C" };
    
    // Normal case: deduplicate, filter allowed, truncate limit
    std::wstring normalized = QuickView::NormalizeCSV(L"A, B, D, C, A, B", allowed, 2);
    EXPECT_EQ(normalized, L"A,B");

    // Under limit
    std::wstring normalized2 = QuickView::NormalizeCSV(L"A, B, D, C, A, B", allowed, 5);
    EXPECT_EQ(normalized2, L"A,B,C");
}

extern void ParseFixedZoomLevels();

TEST(StringUtilsTest, ParseFixedZoomLevelsWithFullWidthCommasAndSpaces) {
    g_config.FixedZoomLevels = L" 0.05 , 0.1 ， 0.15,0.2 , 0.25　,0.3 ， 0.35,0.4, 0.45, 0.5, 0.55, 0.6, 0.65, 0.7, 0.75, 0.8, 0.85, 0.9, 0.95, 1, 1.25, 1.5, 1.75, 2, 4, 8, 10 ";
    ParseFixedZoomLevels();
    EXPECT_EQ(g_runtime.ParsedFixedZoomLevels.size(), 27);
    if (!g_runtime.ParsedFixedZoomLevels.empty()) {
        EXPECT_NEAR(g_runtime.ParsedFixedZoomLevels.front(), 0.05f, 0.0001f);
        EXPECT_NEAR(g_runtime.ParsedFixedZoomLevels.back(), 10.0f, 0.0001f);
    }
}
