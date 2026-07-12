#pragma once

#include <string>
#include <cstdint>
#include <vector>

struct HighScoreTableView {
    std::string id;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    std::vector<std::vector<bool>> isPlaceholder;
};

struct HighScoreView {
    std::vector<HighScoreTableView> tables;
};

struct HighScoreSnapshot {
    HighScoreView view;
    uint64_t revision = 0;
};
