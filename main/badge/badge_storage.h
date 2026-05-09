#pragma once

#include <string>
#include <vector>

struct BadgeMediaItem {
    std::string basename;
    std::string bwp_path;
    std::string wav_path;
    bool has_wav = false;
};

class BadgeStorage {
public:
    bool Mount();
    bool mounted() const { return mounted_; }
    const std::vector<BadgeMediaItem>& Media() const { return media_; }
    std::string NextRecordingPath(int number) const;

private:
    bool EnsureDirectory(const char* path);
    void ScanMedia();
    bool FileExists(const std::string& path) const;

    bool mounted_ = false;
    std::vector<BadgeMediaItem> media_;
};
