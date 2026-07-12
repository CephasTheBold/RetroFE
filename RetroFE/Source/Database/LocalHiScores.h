#pragma once

#include "HighScoreData.h"

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <openhi2txt/openhi2txt.h>

class LocalHiScores {
public:
    static LocalHiScores& getInstance();

    void loadHighScores(const std::string& zipPath, const std::string& overridePath);
    HighScoreData getHighScoreTable(const std::string& gameName);
    HighScoreData getHighScoreTable(const std::string& gameName, bool consumeForceRedraw);
    bool hasHiFile(const std::string& gameName) const;
    bool runHi2Txt(const std::string& gameName);
    void runHi2TxtAsync(const std::string& gameName);
    void deinitialize();

private:
    LocalHiScores() = default;

    std::string hiFilesDirectory_;
    std::string scoresDirectory_;
    std::unique_ptr<openhi2txt::Context> openhi2txtContext_;
    std::unordered_map<std::string, HighScoreData> scoresCache_;
    mutable std::shared_mutex scoresCacheMutex_;
};
