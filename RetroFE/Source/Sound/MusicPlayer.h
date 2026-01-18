/* This file is part of RetroFE.
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RetroFE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <random> // Needed for std::mt19937 member

#if __has_include(<SDL2/SDL_mixer.h>)
#include <SDL2/SDL_mixer.h>
#elif __has_include(<SDL2_mixer/SDL_mixer.h>)
#include <SDL2_mixer/SDL_mixer.h>
#else
#error "Cannot find SDL_mixer header"
#endif

 // Forward declarations to reduce compile-time dependencies
class Configuration;
class MusicPlayerComponent;

class MusicPlayer {
public:
    // Singleton & Basic Setup
    static MusicPlayer* getInstance();

    // Track Metadata Structure
    struct TrackMetadata {
        std::string title;
        std::string artist;
        std::string album;
        std::string year;
        std::string genre;
        std::string comment;
        int trackNumber = 0;
    };

    enum class PlaybackState {
        NONE,
        PLAYING,
        PAUSED,
        NEXT,
        PREVIOUS
    };

    // Initialization & Shutdown
    bool initialize(Configuration& config);
    void reinitialize();
    void onGameLaunchStart();
    void onGameLaunchEnd(bool wasSdlUnloaded);
    void shutdown();
    void pump(); // Main loop update

    // Playlist & Folder Loading
    bool loadM3UPlaylist(const std::string& playlistPath);
    void loadMusicFolderFromConfig();
    bool loadMusicFolder(const std::string& folderPath);

    // Playback Control
    bool playMusic(int index = -1, int customFadeMs = -1, double position = -1);
    bool pauseMusic(int customFadeMs = -1);
    bool resumeMusic(int customFadeMs = -1);
    bool stopMusic(int customFadeMs = -1);
    bool nextTrack(int customFadeMs = -1);
    bool previousTrack(int customFadeMs = -1);

    // State Queries
    bool isPlaying() const;
    bool isPaused() const;
    bool hasStartedPlaying() const;
    bool isFading() const;
    bool hasTrackChanged();
    bool isPlayingNewTrack();
    void setPlaybackState(PlaybackState state) { playbackState_ = state; }
    PlaybackState getPlaybackState() const { return playbackState_; }

    // Audio Processing & Visuals
    void processAudioData(Uint8* stream, int len);
    const std::vector<float>& getAudioLevels() const { return audioLevels_; }
    int getAudioChannels() const { return audioChannels_; }
    int getAudioSampleRate() const { return audioSampleRate_; }
    bool hasVuMeter() const { return hasVuMeter_; }
    void setHasVuMeter(bool enable) { hasVuMeter_ = enable; }
    int getSampleSize() const;
    void addVisualizerListener(MusicPlayerComponent* listener);
    void removeVisualizerListener(MusicPlayerComponent* listener);

    // Volume & Settings
    void changeVolume(bool increase);
    void setVolume(int volume);      // 0-128 (Raw Mixer)
    void setLogicalVolume(int v);    // 0-128 (UI Value -> Curve -> Mixer)
    int getLogicalVolume();
    int getVolume() const;
    void fadeToVolume(int targetLogicalVolume, int customFadeMs = -1);
    void fadeBackToPreviousVolume();
    void setLoop(bool loop);
    bool getLoop() const;

    // Shuffle
    bool shuffle();
    bool setShuffle(bool shuffle);
    bool getShuffle() const;

    // Track Info
    int getCurrentTrackIndex() const;
    int getNextTrackIndex();
    int getTrackCount() const;
    std::string getCurrentTrackName() const;
    std::string getCurrentTrackNameWithoutExtension() const;
    std::string getCurrentTrackPath() const;
    std::string getFormattedTrackInfo(int index = -1) const;

    // Metadata Access
    std::string getTrackArtist(int index = -1) const;
    std::string getTrackAlbum(int index = -1) const;
    const TrackMetadata& getCurrentTrackMetadata() const;
    const TrackMetadata& getTrackMetadata(int index) const;
    size_t getTrackMetadataCount() const;

    // Convenience Metadata Wrappers
    std::string getCurrentTitle() const;
    std::string getCurrentArtist() const;
    std::string getCurrentAlbum() const;
    std::string getCurrentYear() const;
    std::string getCurrentGenre() const;
    std::string getCurrentComment() const;
    int getCurrentTrackNumber() const;

    // Album Art
    bool getAlbumArt(int trackIndex, std::vector<unsigned char>& albumArtData);

    // Position & Duration
    double saveCurrentMusicPosition();
    double getCurrent();
    double getDuration();
    std::pair<int, int> getCurrentAndDurationSec();

    // Input State
    bool getButtonPressed();
    void setButtonPressed(bool buttonPressed);

private:
    MusicPlayer();
    ~MusicPlayer();

    // Internal Types
    enum class FinishEvent : uint8_t {
        None = 0,
        PauseAfterFade,
        TrackChangeAfterFade,
        StopAfterFade,
        NaturalEnd
    };

    struct MusicTransitionFade {
        bool active = false;
        bool fadingOut = false;
        uint64_t startTimeMs = 0;
        int durationMs = 0;
        int startVol = 0;
        int targetVol = 0;

        FinishEvent finishAction = FinishEvent::None;
        int finishIndex = -1;
        double finishSeekPos = -1.0;
        int fadeInMs = 0;
    };

    // Private Helpers
    void loadTrack(int index);
    bool readTrackMetadata(const std::string& filePath, TrackMetadata& metadata) const;
    bool parseM3UFile(const std::string& playlistPath);
    bool isValidAudioFile(const std::string& filePath) const;
    static void musicFinishedCallback();
    int applyVolumeCurve(int logicalVolume) const;
    void beginFadeOutToAction(FinishEvent action, int index, double seekPos, int fadeOutMs, int fadeInMs);
    void beginFadeInToSteadyVolume(int fadeInMs);
    int steadyMusicVolume() const;

    // Members
    static MusicPlayer* instance_;
    Configuration* config_;

    // Playback Data
    Mix_Music* currentMusic_;
    std::vector<std::string> musicFiles_;
    std::vector<std::string> musicNames_;
    std::vector<TrackMetadata> trackMetadata_;

    // Shuffle Data
    std::vector<int> shuffledIndices_;
    int currentShufflePos_;
    std::mt19937 rng_;

    // State
    PlaybackState playbackState_;
    int currentIndex_;
    bool loopMode_;
    bool shuffleMode_;
    bool hasStartedPlaying_;
    int fadeMs_;
    std::string lastCheckedTrackPath_;
    int savedTrackIndex_;
    double savedPosition_;

    // Volume State
    std::atomic<int> volume_;          // Actual mixer vol
    std::atomic<int> logicalVolume_;   // UI bar vol
    std::atomic<int> previousLogicalVolume_;

    // Volume Fading (In-Game / User volume change)
    bool isVolumeFading_ = false;
    uint64_t volumeFadeStartTime_ = 0;
    int volumeFadeDuration_;
    int volumeFadeStartVal_;
    int volumeFadeTargetVal_;
    Uint64 lastVolumeChangeTime_;
    Uint64 volumeChangeIntervalMs_;
    bool buttonPressed_;

    // Transition Fading
    MusicTransitionFade musicFade_;
    std::atomic<FinishEvent> finishEvent_{ FinishEvent::None };
    std::atomic<int> ignoreFinishCallbacks_{ 0 };
    std::atomic<bool> isShuttingDown_;

    // Visualization
    std::vector<MusicPlayerComponent*> visualizerListeners_;
    std::mutex visualizerMutex_;
    bool hasActiveVisualizers_ = false;
    std::vector<float> audioLevels_;
    int audioChannels_;
    int audioSampleRate_;
    bool hasVuMeter_;
    int sampleSize_;
};