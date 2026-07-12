#pragma once

#include <string>
#include <vector>

struct HighScoreTable {
    std::string id;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    std::vector<std::vector<bool>> isPlaceholder;
    bool forceRedraw = false;
};

struct HighScoreData {
    std::vector<HighScoreTable> tables;
};
