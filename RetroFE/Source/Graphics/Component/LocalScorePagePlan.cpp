#include "LocalScorePagePlan.h"

#include <algorithm>
#include <utility>

namespace {
bool canPair(const LocalTableCandidate& left, const LocalTableCandidate& right,
    const LocalScorePagePolicy& policy) {
    if (!left.displayable || !right.displayable) return false;
    if (left.halfWidthRenderedFontPx < policy.minimumReadableFontPx ||
        right.halfWidthRenderedFontPx < policy.minimumReadableFontPx) return false;

    const size_t leftOverflow = left.overflowRows();
    const size_t rightOverflow = right.overflowRows();
    if (leftOverflow == 0 || rightOverflow == 0) return true;

    const float similarity = static_cast<float>(std::min(leftOverflow, rightOverflow)) /
        static_cast<float>(std::max(leftOverflow, rightOverflow));
    return similarity >= policy.minimumScrollingSimilarity;
}
}

std::vector<LocalScorePagePlan> buildLocalScorePagePlan(
    const std::vector<LocalTableCandidate>& candidates,
    const LocalScorePagePolicy& policy) {
    std::vector<LocalScorePagePlan> pages;
    for (size_t i = 0; i < candidates.size();) {
        if (!candidates[i].displayable) { ++i; continue; }

        LocalScorePagePlan page;
        page.tableIndices.push_back(candidates[i].tableIndex);
        size_t next = i + 1;
        while (next < candidates.size() && !candidates[next].displayable) ++next;

        if (next < candidates.size() && canPair(candidates[i], candidates[next], policy)) {
            page.tableIndices.push_back(candidates[next].tableIndex);
            i = next + 1;
        }
        else {
            ++i;
        }
        pages.push_back(std::move(page));
    }
    return pages;
}
