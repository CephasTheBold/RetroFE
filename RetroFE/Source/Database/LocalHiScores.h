#pragma once

#include "HighScoreView.h"

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <openhi2txt/openhi2txt.h>

struct LocalScoreQuery {
    std::string gameName;
    bool consumeForceRedraw = false;
};

class LocalHiScores {
public:
    static LocalHiScores& getInstance();

    void loadHighScores(const std::string& zipPath, const std::string& overridePath);
    HighScoreView getTable(const LocalScoreQuery& query);
    bool hasHiFile(const std::string& gameName) const;
    bool runHi2Txt(const std::string& gameName);
    void runHi2TxtAsync(const std::string& gameName);
    void deinitialize();

private:
    LocalHiScores() = default;

    std::string hiFilesDirectory_;
    std::string scoresDirectory_;
    std::unique_ptr<openhi2txt::Context> openhi2txtContext_;
    std::unordered_map<std::string, HighScoreView> scoresCache_;
    mutable std::shared_mutex scoresCacheMutex_;
};
