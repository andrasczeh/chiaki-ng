// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Marks chiaki-ng's Steam Frame theatre screen as half side-by-side stereo while
// synthetic 3D is active, so each eye gets its half of the picture without any
// external tool. Same OpenVR overlay calls as MichaelScottsman/frame-sbs-toggle
// (VROverlayFlags_SideBySide_Parallel + doubled texel aspect), done in-process.
//
// The SteamVR runtime's libopenvr_api.so is loaded at run time; when it, SteamVR or
// the screen can't be found this just logs why and does nothing, and the stream
// stays a plain (flat) side-by-side picture.

#ifndef CHIAKI_FRAMESTEREOOVERLAY_H
#define CHIAKI_FRAMESTEREOOVERLAY_H

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class FrameStereoOverlay
{
public:
    FrameStereoOverlay();
    ~FrameStereoOverlay(); // restores the screen (bounded wait) and stops the worker
    FrameStereoOverlay(const FrameStereoOverlay &) = delete;
    FrameStereoOverlay &operator=(const FrameStereoOverlay &) = delete;

    // Thread-safe, non-blocking. true while a half-SBS 3D stream is running.
    void setWanted(bool wanted);

    // Last status, for logs/UI
    std::string status() const;

    struct Impl;

private:
    void run();

    std::unique_ptr<Impl> impl;
    mutable std::mutex mutex;
    std::condition_variable cond;
    bool wanted = false;
    bool quit = false;
    std::string status_text = "off";
    std::thread worker;
};

#endif // CHIAKI_FRAMESTEREOOVERLAY_H
