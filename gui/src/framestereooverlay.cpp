// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "framestereooverlay.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>

#include <chrono>
#include <cmath>
#include <dlfcn.h>
#include <vector>

#include "../../third-party/openvr/openvr.h"

Q_DECLARE_LOGGING_CATEGORY(chiakiGui);

namespace {

using InitInternal2Fn = uint32_t (*)(vr::EVRInitError *, vr::EVRApplicationType, const char *);
using ShutdownInternalFn = void (*)();
using GetGenericInterfaceFn = void *(*)(const char *, vr::EVRInitError *);
using IsInterfaceVersionValidFn = bool (*)(const char *);
using InitErrorDescFn = const char *(*)(vr::EVRInitError);

struct OverlayState
{
    bool parallel = false;
    bool crossed = false;
    float aspect = 1.0f;
};

bool sameState(const OverlayState &a, const OverlayState &b)
{
    return a.parallel == b.parallel && a.crossed == b.crossed && std::fabs(a.aspect - b.aspect) < 0.0001f;
}

QStringList runtimeLibraryCandidates()
{
    QStringList out;
    const QString env = qEnvironmentVariable("CHIAKI_OPENVR_LIB");
    if (!env.isEmpty())
        out << env;
    const QString override_root = qEnvironmentVariable("VR_OVERRIDE");
    if (!override_root.isEmpty()) {
        out << override_root + QStringLiteral("/bin/linuxarm64/libopenvr_api.so")
            << override_root + QStringLiteral("/bin/linux64/libopenvr_api.so");
    }
    // Steam Frame (SteamOS ARM64)
    out << QStringLiteral("/opt/steamvr/bin/linuxarm64/libopenvr_api.so");
    // Desktop Linux with SteamVR installed through Steam
    const QString home = QDir::homePath();
    out << home + QStringLiteral("/.steam/steam/steamapps/common/SteamVR/bin/linux64/libopenvr_api.so")
        << home + QStringLiteral("/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/libopenvr_api.so");
    return out;
}

// Steam passes the launched app's id; gamescope names the theatre screen after it.
// Non-Steam shortcuts get a 64-bit game id whose upper half is the 32-bit app id.
QStringList overlayKeyCandidates()
{
    QStringList keys;
    const QString explicit_key = qEnvironmentVariable("CHIAKI_FRAME_OVERLAY_KEY");
    if (!explicit_key.isEmpty())
        keys << explicit_key;
    auto add_id = [&keys](const QString &value) {
        bool ok = false;
        const quint64 id = value.trimmed().toULongLong(&ok);
        if (!ok || id == 0)
            return;
        keys << QStringLiteral("valve.steam.desktopgame.%1").arg(id);
        if (id > 0xffffffffull)
            keys << QStringLiteral("valve.steam.desktopgame.%1").arg(id >> 32);
    };
    for (const char *var : {"SteamAppId", "SteamGameId", "SteamOverlayGameId", "STEAM_COMPAT_APP_ID"})
        add_id(qEnvironmentVariable(var));
    keys << QStringLiteral("valve.steam.desktopgame");
    keys.removeDuplicates();
    return keys;
}

} // namespace

struct FrameStereoOverlay::Impl
{
    void *lib = nullptr;
    bool lib_failed = false;
    QString lib_path;
    InitInternal2Fn init = nullptr;
    ShutdownInternalFn shutdown = nullptr;
    GetGenericInterfaceFn get_interface = nullptr;
    IsInterfaceVersionValidFn interface_valid = nullptr;
    InitErrorDescFn init_error_desc = nullptr;

    bool vr_open = false;
    vr::IVROverlay *overlay = nullptr;

    bool applied = false;
    std::string key;
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    OverlayState original;
    OverlayState target;

    ~Impl()
    {
        closeVr();
        if (lib)
            dlclose(lib);
    }

    bool loadLibrary(std::string &why)
    {
        if (lib)
            return true;
        if (lib_failed) {
            why = "SteamVR runtime not found";
            return false;
        }
        QString tried;
        for (const QString &path : runtimeLibraryCandidates()) {
            if (!QFileInfo::exists(path))
                continue;
            void *h = dlopen(path.toLocal8Bit().constData(), RTLD_NOW | RTLD_LOCAL);
            if (!h) {
                tried += QStringLiteral(" %1 (%2)").arg(path, QString::fromLocal8Bit(dlerror()));
                continue;
            }
            init = reinterpret_cast<InitInternal2Fn>(dlsym(h, "VR_InitInternal2"));
            shutdown = reinterpret_cast<ShutdownInternalFn>(dlsym(h, "VR_ShutdownInternal"));
            get_interface = reinterpret_cast<GetGenericInterfaceFn>(dlsym(h, "VR_GetGenericInterface"));
            interface_valid = reinterpret_cast<IsInterfaceVersionValidFn>(dlsym(h, "VR_IsInterfaceVersionValid"));
            init_error_desc = reinterpret_cast<InitErrorDescFn>(dlsym(h, "VR_GetVRInitErrorAsEnglishDescription"));
            if (!init || !shutdown || !get_interface || !interface_valid) {
                tried += QStringLiteral(" %1 (missing symbols)").arg(path);
                dlclose(h);
                continue;
            }
            lib = h;
            lib_path = path;
            qCInfo(chiakiGui) << "3D: loaded OpenVR runtime" << path;
            return true;
        }
        lib_failed = true;
        why = tried.isEmpty() ? "SteamVR runtime not found" : ("could not load SteamVR runtime:" + tried.toStdString());
        return false;
    }

    bool openVr(std::string &why)
    {
        if (vr_open)
            return true;
        vr::EVRInitError err = vr::VRInitError_None;
        init(&err, vr::VRApplication_Background, nullptr);
        if (err != vr::VRInitError_None) {
            why = std::string("SteamVR not reachable: ")
                + (init_error_desc ? init_error_desc(err) : std::to_string(static_cast<int>(err)));
            return false;
        }
        vr_open = true;
        if (!interface_valid(vr::IVROverlay_Version)) {
            why = std::string("SteamVR runtime lacks ") + vr::IVROverlay_Version;
            closeVr();
            return false;
        }
        overlay = static_cast<vr::IVROverlay *>(get_interface(vr::IVROverlay_Version, &err));
        if (!overlay || err != vr::VRInitError_None) {
            why = "OpenVR overlay interface unavailable";
            closeVr();
            return false;
        }
        return true;
    }

    void closeVr()
    {
        overlay = nullptr;
        if (vr_open && shutdown)
            shutdown();
        vr_open = false;
    }

    bool readState(vr::VROverlayHandle_t h, OverlayState &s)
    {
        return overlay->GetOverlayFlag(h, vr::VROverlayFlags_SideBySide_Parallel, &s.parallel) == vr::VROverlayError_None
            && overlay->GetOverlayFlag(h, vr::VROverlayFlags_SideBySide_Crossed, &s.crossed) == vr::VROverlayError_None
            && overlay->GetOverlayTexelAspect(h, &s.aspect) == vr::VROverlayError_None;
    }

    bool writeState(vr::VROverlayHandle_t h, const OverlayState &s)
    {
        // Same order as frame-sbs-toggle: clear stereo, set aspect, then the flags.
        return overlay->SetOverlayFlag(h, vr::VROverlayFlags_SideBySide_Parallel, false) == vr::VROverlayError_None
            && overlay->SetOverlayFlag(h, vr::VROverlayFlags_SideBySide_Crossed, false) == vr::VROverlayError_None
            && overlay->SetOverlayTexelAspect(h, s.aspect) == vr::VROverlayError_None
            && overlay->SetOverlayFlag(h, vr::VROverlayFlags_SideBySide_Parallel, s.parallel) == vr::VROverlayError_None
            && overlay->SetOverlayFlag(h, vr::VROverlayFlags_SideBySide_Crossed, s.crossed) == vr::VROverlayError_None;
    }

    // Keys reported by SteamVR's vrcmd, if it can run here (often not inside a Flatpak).
    QStringList vrcmdVisibleKeys()
    {
        QStringList keys;
        QDir bin = QFileInfo(lib_path).dir();
        const QString vrcmd = bin.filePath(QStringLiteral("vrcmd"));
        if (!QFileInfo(vrcmd).isExecutable())
            return keys;
        QProcess p;
        p.start(vrcmd, {QStringLiteral("--overlays")});
        if (!p.waitForFinished(5000) || p.exitStatus() != QProcess::NormalExit)
            return keys;
        static const QRegularExpression re(QStringLiteral("^'(valve\\.steam\\.desktopgame(?:\\.[0-9]+)?)' -- "));
        for (const QString &line : QString::fromLocal8Bit(p.readAllStandardOutput()).split('\n')) {
            const auto m = re.match(line);
            if (m.hasMatch() && (line + ' ').contains(QStringLiteral(" visible ")))
                keys << m.captured(1);
        }
        return keys;
    }

    // Finds chiaki-ng's own theatre screen: prefer a visible match.
    bool findScreen(std::string &found_key, vr::VROverlayHandle_t &found, std::string &why)
    {
        QStringList candidates = overlayKeyCandidates();
        QString fallback_key;
        vr::VROverlayHandle_t fallback = vr::k_ulOverlayHandleInvalid;
        for (int pass = 0; pass < 2; pass++) {
            for (const QString &k : candidates) {
                vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
                if (overlay->FindOverlay(k.toUtf8().constData(), &h) != vr::VROverlayError_None
                    || h == vr::k_ulOverlayHandleInvalid)
                    continue;
                if (overlay->IsOverlayVisible(h)) {
                    found_key = k.toStdString();
                    found = h;
                    return true;
                }
                if (fallback == vr::k_ulOverlayHandleInvalid) {
                    fallback_key = k;
                    fallback = h;
                }
            }
            if (pass == 0) {
                // Nothing visible among the guessed keys: ask vrcmd. Only trust it
                // when exactly one game screen is visible.
                const QStringList visible = vrcmdVisibleKeys();
                if (visible.size() != 1)
                    break;
                candidates = visible;
            }
        }
        if (fallback != vr::k_ulOverlayHandleInvalid) {
            found_key = fallback_key.toStdString();
            found = fallback;
            return true;
        }
        why = "theatre screen not found (tried " + candidates.join(QStringLiteral(", ")).toStdString()
            + "); set CHIAKI_FRAME_OVERLAY_KEY to override";
        return false;
    }
};

FrameStereoOverlay::FrameStereoOverlay()
    : impl(std::make_unique<Impl>())
{
    // Inside a Flatpak, XDG_CONFIG_HOME points into the sandbox, so the runtime
    // registry (openvrpaths.vrpath) isn't found; point OpenVR at the runtime directly.
    // Done here, on the creating thread, before the worker exists.
    if (qEnvironmentVariableIsEmpty("VR_OVERRIDE")) {
        for (const QString &path : runtimeLibraryCandidates()) {
            if (!QFileInfo::exists(path))
                continue;
            QDir root = QFileInfo(path).dir(); // <runtime>/bin/<platform>
            root.cdUp();
            root.cdUp();
            qputenv("VR_OVERRIDE", root.absolutePath().toLocal8Bit());
            break;
        }
    }
    worker = std::thread([this]() { run(); });
}

FrameStereoOverlay::~FrameStereoOverlay()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        wanted = false;
        quit = true;
    }
    cond.notify_all();
    if (worker.joinable())
        worker.join();
}

void FrameStereoOverlay::setWanted(bool w)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (wanted == w)
            return;
        wanted = w;
    }
    cond.notify_all();
}

std::string FrameStereoOverlay::status() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return status_text;
}

void FrameStereoOverlay::run()
{
    using namespace std::chrono_literals;
    Impl &d = *impl;
    std::string last_logged;
    int attempts = 0;

    auto set_status = [&](const std::string &s, bool warn) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            status_text = s;
        }
        if (s != last_logged) {
            last_logged = s;
            if (warn)
                qCWarning(chiakiGui) << "3D: Steam Frame stereo screen:" << QString::fromStdString(s);
            else
                qCInfo(chiakiGui) << "3D: Steam Frame stereo screen:" << QString::fromStdString(s);
        }
    };

    auto restore = [&]() {
        if (!d.applied)
            return;
        std::string why;
        if (d.openVr(why)) {
            vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
            OverlayState now;
            if (d.overlay->FindOverlay(d.key.c_str(), &h) == vr::VROverlayError_None && h == d.handle
                && d.readState(h, now) && sameState(now, d.target)) {
                if (!d.writeState(h, d.original))
                    qCWarning(chiakiGui) << "3D: failed to restore the theatre screen";
            }
            // A recreated or changed screen no longer carries our settings: leave it.
        }
        d.applied = false;
        d.closeVr();
        set_status("off", false);
    };

    std::unique_lock<std::mutex> lock(mutex);
    while (true) {
        const bool want = wanted;
        const bool stop = quit;
        lock.unlock();

        if (stop) {
            restore();
            return;
        }

        std::chrono::milliseconds wait = 2000ms;
        if (!want) {
            restore();
            set_status("off", false);
            attempts = 0;
            wait = 24h;
        } else {
            std::string why;
            if (!d.loadLibrary(why)) {
                set_status(why, false); // not a Frame / no SteamVR: nothing to do
                wait = 24h;
            } else if (!d.openVr(why)) {
                set_status(why, attempts == 0);
                attempts++;
            } else if (d.applied) {
                // Gamescope can recreate the screen (e.g. after a window remap): re-apply.
                vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
                OverlayState now;
                if (d.overlay->FindOverlay(d.key.c_str(), &h) != vr::VROverlayError_None || h != d.handle
                    || !d.readState(h, now) || !sameState(now, d.target)) {
                    qCInfo(chiakiGui) << "3D: theatre screen changed, applying side-by-side again";
                    d.applied = false;
                    wait = 0ms;
                }
            } else {
                std::string key;
                vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
                OverlayState original;
                if (!d.findScreen(key, h, why)) {
                    // The window may not be mapped yet right after the stream starts.
                    set_status(why, attempts == 10);
                    attempts++;
                    wait = attempts < 10 ? 500ms : 3000ms;
                } else if (!d.readState(h, original)) {
                    set_status("could not read theatre screen settings", true);
                } else {
                    if (original.parallel || original.crossed) {
                        // Already stereo (e.g. frame-sbs-toggle): don't stack on top of it.
                        set_status("screen is already in stereo mode, leaving it alone", false);
                        wait = 5000ms;
                    } else {
                        OverlayState target = original;
                        target.parallel = true;
                        target.crossed = false;
                        target.aspect = original.aspect * 2.0f; // half SBS: stretch each eye back
                        if (!d.writeState(h, target)) {
                            d.writeState(h, original);
                            set_status("SteamVR rejected the stereo settings", true);
                            wait = 5000ms;
                        } else {
                            std::this_thread::sleep_for(300ms);
                            OverlayState check;
                            if (!d.readState(h, check) || !sameState(check, target)) {
                                d.writeState(h, original);
                                set_status("stereo settings were reset by the compositor", true);
                                wait = 5000ms;
                            } else {
                                d.applied = true;
                                d.key = key;
                                d.handle = h;
                                d.original = original;
                                d.target = target;
                                attempts = 0;
                                set_status("half side-by-side on " + key, false);
                            }
                        }
                    }
                }
            }
        }

        lock.lock();
        if (wanted == want && !quit)
            cond.wait_for(lock, wait, [&] { return wanted != want || quit; });
    }
}
