#pragma once

#include <cstddef>
#include <vector>

struct LocalTableCandidate {
    size_t tableIndex = 0;
    float halfWidthRenderedFontPx = 0.0f;
    size_t visibleRows = 0;
    size_t totalRows = 0;
    bool displayable = false;
    size_t overflowRows() const { return totalRows > visibleRows ? totalRows - visibleRows : 0; }
};

struct LocalScorePagePlan { std::vector<size_t> tableIndices; };

struct LocalScorePagePolicy {
    float minimumReadableFontPx = 28.0f;
    float minimumScrollingSimilarity = 0.65f;
};

std::vector<LocalScorePagePlan> buildLocalScorePagePlan(
    const std::vector<LocalTableCandidate>& candidates,
    const LocalScorePagePolicy& policy = {});
