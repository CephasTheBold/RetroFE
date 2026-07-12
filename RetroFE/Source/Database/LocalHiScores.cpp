#include "LocalHiScores.h"

#include "Configuration.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"

#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>

namespace {
constexpr const char* kOpenHi2txtObfuscationKey = "s3cReT123!";

HighScoreData toRetroFeHighScoreData(const openhi2txt::HiScoreResult& result) {
    HighScoreData data;
    data.tables.reserve(result.tables.size());
    for (const auto& sourceTable : result.tables) {
        HighScoreTable table;
        table.id = sourceTable.id;
        table.columns = sourceTable.columns;
        table.rows = sourceTable.rows;
        table.isPlaceholder.assign(table.rows.size(), std::vector<bool>(table.columns.size(), false));
        table.forceRedraw = true;
        data.tables.push_back(std::move(table));
    }
    return data;
}
}

LocalHiScores& LocalHiScores::getInstance() {
    static LocalHiScores instance;
    return instance;
}

void LocalHiScores::deinitialize() {
    {
        std::unique_lock<std::shared_mutex> lock(scoresCacheMutex_);
        scoresCache_.clear();
    }
    openhi2txtContext_.reset();
    hiFilesDirectory_.clear();
    scoresDirectory_.clear();
    LOG_INFO("LocalHiScores", "Local high scores deinitialized and cache cleared.");
}

void LocalHiScores::loadHighScores(const std::string& zipPath, const std::string& overridePath) {
    hiFilesDirectory_ = Utils::combinePath(Configuration::absolutePath, "emulators", "mame", "hiscore");
    scoresDirectory_ = overridePath;

    openhi2txt::ContextOptions options;
    options.definitionsZip = Utils::combinePath(Configuration::absolutePath, "hi2txt", "hi2txt.zip");
    options.defaultsZip = zipPath;
    options.scoresDirectory = scoresDirectory_;
    options.mameRoot = Utils::combinePath(Configuration::absolutePath, "emulators", "mame");
    options.defaults.obfuscation = openhi2txt::ObfuscationMode::Xor;
    options.defaults.key = kOpenHi2txtObfuscationKey;
    options.scoreCache.obfuscation = openhi2txt::ObfuscationMode::Xor;
    options.scoreCache.key = kOpenHi2txtObfuscationKey;

    try {
        openhi2txtContext_ = std::make_unique<openhi2txt::Context>(std::move(options));
    }
    catch (const std::exception& e) {
        LOG_ERROR("LocalHiScores", std::string("Failed to initialize OpenHi2txt: ") + e.what());
        return;
    }

    if (!std::filesystem::exists(overridePath) || !std::filesystem::is_directory(overridePath)) {
        LOG_INFO("LocalHiScores", "Score override directory does not exist yet: " + overridePath);
    }

    const auto persistedScores = openhi2txtContext_->readAllPersistedGames();
    int loaded = 0;
    {
        std::unique_lock<std::shared_mutex> lock(scoresCacheMutex_);
        scoresCache_.clear();
        for (const auto& persistedScore : persistedScores) {
            HighScoreData data = toRetroFeHighScoreData(persistedScore.second);
            if (data.tables.empty()) continue;
            scoresCache_[persistedScore.first] = std::move(data);
            ++loaded;
        }
    }
    LOG_INFO("LocalHiScores", "OpenHi2txt local cache bulk-loaded " + std::to_string(loaded) + " games.");
}

HighScoreData LocalHiScores::getHighScoreTable(const std::string& gameName) {
    return getHighScoreTable(gameName, false);
}

HighScoreData LocalHiScores::getHighScoreTable(const std::string& gameName, bool consumeForceRedraw) {
    std::unique_lock<std::shared_mutex> lock(scoresCacheMutex_);
    auto it = scoresCache_.find(gameName);
    if (it == scoresCache_.end()) return {};
    HighScoreData result = it->second;
    if (consumeForceRedraw) {
        for (auto& table : it->second.tables) table.forceRedraw = false;
    }
    return result;
}

bool LocalHiScores::hasHiFile(const std::string& gameName) const {
    if (openhi2txtContext_) return openhi2txtContext_->hasInputForGame(gameName);
    return std::filesystem::exists(Utils::combinePath(hiFilesDirectory_, gameName + ".hi"));
}

bool LocalHiScores::runHi2Txt(const std::string& gameName) {
    if (!openhi2txtContext_) {
        LOG_ERROR("LocalHiScores", "OpenHi2txt context is not initialized; cannot refresh " + gameName);
        return false;
    }
    if (!openhi2txtContext_->hasInputForGame(gameName)) {
        LOG_INFO("LocalHiScores", "No hi/nvram input exists for " + gameName + ", skipping OpenHi2txt refresh.");
        return false;
    }

    openhi2txt::HiScoreResult result = openhi2txtContext_->refreshGame(gameName);
    if (!result.ok) {
        LOG_WARNING("LocalHiScores", "OpenHi2txt refresh failed for " + gameName + ": " + result.error);
        return false;
    }

    HighScoreData data = toRetroFeHighScoreData(result);
    if (data.tables.empty()) {
        LOG_WARNING("LocalHiScores", "OpenHi2txt produced no display tables for " + gameName);
        return false;
    }
    {
        std::unique_lock<std::shared_mutex> lock(scoresCacheMutex_);
        scoresCache_[gameName] = std::move(data);
    }
    LOG_INFO("LocalHiScores", "Scores updated for " + gameName + " using OpenHi2txt.");
    return true;
}

void LocalHiScores::runHi2TxtAsync(const std::string& gameName) {
    if (!hasHiFile(gameName)) {
        LOG_INFO("LocalHiScores", "No hi/nvram input exists for " + gameName + ", skipping async OpenHi2txt refresh.");
        return;
    }
    std::thread([this, gameName]() {
        try {
            if (runHi2Txt(gameName)) {
                LOG_INFO("LocalHiScores", "OpenHi2txt refresh executed successfully in the background for game " + gameName);
            } else {
                LOG_ERROR("LocalHiScores", "OpenHi2txt refresh failed in the background for game " + gameName);
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR("LocalHiScores", "Exception in async OpenHi2txt refresh for game " + gameName + ": " + e.what());
        }
        catch (...) {
            LOG_ERROR("LocalHiScores", "Unknown exception in async OpenHi2txt refresh for game " + gameName);
        }
    }).detach();
}
