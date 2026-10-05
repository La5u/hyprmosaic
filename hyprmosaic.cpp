// Per-workspace wallpapers that slide with their workspace, plus grid workspace swipes, for Hyprland.
// SPDX-License-Identifier: MIT
//
// The swipe commit/cancel logic is adapted from Hyprland's UnifiedWorkspaceSwipeGesture
// (BSD-3-Clause, Copyright (c) 2022-2026, vaxerski). Inspired by jairnarvaez/Hyprgrid.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <sys/inotify.h>
#include <unistd.h>

#include <hyprgraphics/image/Image.hpp>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/Compositor.hpp>

static HANDLE PHANDLE = nullptr;
static Hyprutils::Signal::CHyprSignalListener beginListener, updateListener, endListener, renderListener;
static SP<Config::Values::CBoolValue> blurFill;
static SP<Config::Values::CIntValue> columnsCfg, rowsCfg, fingersCfg;

// Config values, with defaults in case registration failed.
static bool cfgBlurFill() { return !blurFill || blurFill->value(); }
static int cfgColumns() { return columnsCfg ? std::clamp<int>(columnsCfg->value(), 1, 10) : 3; }
static int cfgRows() { return rowsCfg ? std::clamp<int>(rowsCfg->value(), 1, 10) : 3; }
static int cfgFingers() { return fingersCfg ? fingersCfg->value() : 3; }

struct GridSwipe {
    bool tracking = false;
    bool active = false;
    bool vertical = false;
    int columns = 3, rows = 3; // grid size, fixed for the duration of a swipe
    Vector2D total{};
    double delta = 0;
    double averageSpeed = 0;
    int speedPoints = 0;
    PHLWORKSPACE start;
    PHLWORKSPACE held; // workspaces are weakly tracked, so keep a newly created target alive during the swipe
    PHLMONITORREF monitor;

    // Workspaces 1..columns*rows, laid out row by row.
    bool valid(int id) const { return id >= 1 && id <= columns * rows; }
    int negativeID() const {
        const int id = start->m_id;
        if (vertical) return id > columns ? id - columns : id;
        return (id - 1) % columns > 0 ? id - 1 : id;
    }
    int positiveID() const {
        const int id = start->m_id;
        if (vertical) return id <= columns * (rows - 1) ? id + columns : id;
        return (id - 1) % columns < columns - 1 ? id + 1 : id;
    }
    PHLWORKSPACE get(int id) const {
        return valid(id) ? State::workspaceState()->query().id(id).run() : nullptr;
    }
    PHLWORKSPACE ensure(int id) {
        if (auto ws = get(id)) return ws;
        return State::workspaceState()->create(id, monitor->m_id);
    }
    double axisDistance() const {
        static auto gap = CConfigValue<Config::INTEGER>("general:gaps_workspaces");
        return vertical ? monitor->m_size.y + *gap : monitor->m_size.x + *gap;
    }
    Vector2D offset(double value) const {
        return vertical ? Vector2D{0.0, value} : Vector2D{value, 0.0};
    }

    // A target is reachable if it isn't the start and doesn't live on another monitor.
    bool reachable(int id) const {
        if (id == start->m_id) return false;
        const auto ws = get(id);
        return !ws || ws->m_monitor == monitor;
    }

    void begin() {
        const auto focused = Desktop::focusState()->monitor();
        if (!focused || !focused->m_activeWorkspace) return;
        tracking = true;
        columns = cfgColumns();
        rows = cfgRows();
        active = false;
        total = {};
        delta = 0;
        averageSpeed = 0;
        speedPoints = 0;
        start = focused->m_activeWorkspace;
        monitor = focused;
    }

    void update(const IPointer::SSwipeUpdateEvent& e) {
        if (!tracking || !start || !monitor) return;
        total += e.delta;

        // Lock to the dominant physical axis once enough motion is available.
        if (!active) {
            if (std::abs(total.x) < 5 && std::abs(total.y) < 5) return;
            vertical = std::abs(total.y) > std::abs(total.x);
            active = valid(start->m_id);
            if (!active) return;
        }

        static auto distanceCfg = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_distance");
        static auto invert = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_invert");
        const double distance = std::max<int64_t>(1, *distanceCfg);
        const double movement = vertical ? e.delta.y : e.delta.x;
        const double old = delta;
        delta += *invert ? -movement : movement;
        delta = std::clamp(delta, -distance, distance);
        ++speedPoints;
        averageSpeed = (averageSpeed * (speedPoints - 1) + std::abs(delta - old)) / speedPoints;

        const int targetID = delta < 0 ? negativeID() : positiveID();
        if (!reachable(targetID)) {
            if (held && held != start) {
                held->m_forceRendering = false;
                held->m_alpha->setValueAndWarp(0.F);
            }
            held = nullptr;
            delta = 0;
            start->m_renderOffset->setValueAndWarp({});
            g_pHyprRenderer->damageMonitor(monitor.lock());
            return;
        }

        auto target = held = ensure(targetID);
        const int oppositeID = delta < 0 ? positiveID() : negativeID();
        auto opposite = get(oppositeID);
        if (opposite && opposite != start && opposite != target) {
            opposite->m_forceRendering = false;
            opposite->m_alpha->setValueAndWarp(0.F);
        }

        start->m_forceRendering = true;
        target->m_forceRendering = true;
        target->m_alpha->setValueAndWarp(1.F);
        const double screen = axisDistance();
        const double currentOffset = (-delta / distance) * screen;
        const double targetOffset = currentOffset + (delta < 0 ? -screen : screen);
        start->m_renderOffset->setValueAndWarp(offset(currentOffset));
        target->m_renderOffset->setValueAndWarp(offset(targetOffset));
        start->updateWindowDecos();
        target->updateWindowDecos();
        g_pHyprRenderer->damageMonitor(monitor.lock());
    }

    void end(bool cancelled) {
        if (!tracking) return;
        tracking = false;
        if (!active || !start || !monitor) {
            held = nullptr;
            active = false;
            return;
        }

        static auto ratio = CConfigValue<Config::FLOAT>("gestures:workspace_swipe_cancel_ratio");
        static auto distanceCfg = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_distance");
        static auto force = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_min_speed_to_force");
        const double distance = std::max<int64_t>(1, *distanceCfg);
        const int targetID = delta < 0 ? negativeID() : positiveID();
        auto negative = get(negativeID());
        auto positive = get(positiveID());
        const bool cancel = cancelled || !reachable(targetID) || std::abs(delta) < 2 ||
            (std::abs(delta) < distance * *ratio && (*force == 0 || averageSpeed < *force));

        if (cancel) {
            if (negative && negative != start) *negative->m_renderOffset = offset(-axisDistance());
            if (positive && positive != start) *positive->m_renderOffset = offset(axisDistance());
            *start->m_renderOffset = Vector2D{};
        } else {
            auto target = ensure(targetID);
            const auto oldTargetOffset = target->m_renderOffset->value();
            monitor->changeWorkspace(targetID);
            target->m_renderOffset->setValue(oldTargetOffset);
            target->m_alpha->setValueAndWarp(1.F);
            start->m_renderOffset->setValue(start->m_renderOffset->value());
            *start->m_renderOffset = offset(delta < 0 ? axisDistance() : -axisDistance());
            start->m_alpha->setValueAndWarp(1.F);
            g_pInputManager->unconstrainMouse();
        }

        if (negative) negative->m_forceRendering = false;
        if (positive) positive->m_forceRendering = false;
        start->m_forceRendering = false;
        g_pHyprRenderer->damageMonitor(monitor.lock());
        g_pInputManager->refocus();
        start = nullptr;
        held = nullptr;
        active = false;
    }
} swipe;

// Per-workspace wallpapers: $XDG_STATE_HOME/hyprmosaic/<workspace id> (an image or a symlink to one).
// Drawn as part of each workspace, so they slide with it during swipes and switch animations.
struct GridWallpapers {
    struct Wallpaper {
        SP<Render::ITexture> image, blurred;
    };
    std::filesystem::path dir;
    std::unordered_map<int, Wallpaper> walls;
    std::set<int> dirty;
    int notifyFd = -1;
    wl_event_source* source = nullptr;

    void init() {
        const char* state = getenv("XDG_STATE_HOME");
        const char* home = getenv("HOME");
        dir = std::filesystem::path(state && *state ? std::string(state) : std::string(home ? home : "") + "/.local/state") / "hyprmosaic";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
            if (const int id = parseID(entry.path().filename().string())) dirty.insert(id);

        notifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (notifyFd < 0) return;
        inotify_add_watch(notifyFd, dir.c_str(), IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE);
        source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, notifyFd, WL_EVENT_READABLE, onNotify, this);
    }

    void exit() {
        if (source) wl_event_source_remove(source);
        if (notifyFd >= 0) close(notifyFd);
        walls.clear();
    }

    // Workspace id from a file name, or 0 if it isn't one.
    static int parseID(std::string_view name) {
        int id = 0;
        const auto [end, ec] = std::from_chars(name.data(), name.data() + name.size(), id);
        return ec == std::errc{} && end == name.data() + name.size() && id > 0 ? id : 0;
    }

    // Mark changed entries; they are reloaded on the next frame, where the GL context is current.
    static int onNotify(int fd, uint32_t, void* data) {
        auto* self = static_cast<GridWallpapers*>(data);
        alignas(inotify_event) char buf[4096];
        ssize_t len;
        while ((len = read(fd, buf, sizeof(buf))) > 0) {
            for (char* p = buf; p < buf + len;) {
                const auto* e = reinterpret_cast<inotify_event*>(p);
                if (e->len)
                    if (const int id = parseID(e->name)) self->dirty.insert(id);
                p += sizeof(inotify_event) + e->len;
            }
        }
        for (const auto& m : State::monitorState()->monitors()) g_pHyprRenderer->damageMonitor(m);
        return 0;
    }

    void load(int id) {
        walls.erase(id);
        std::error_code ec;
        const auto path = std::filesystem::canonical(dir / std::to_string(id), ec);
        if (ec) return;
        Hyprgraphics::CImage image(path.string());
        if (!image.success()) return;
        auto* src = image.cairoSurface()->cairo();
        auto& wall = walls[id];
        wall.image = g_pHyprRenderer->createTexture(src);

        // Blurred fill: a tiny copy that linear filtering smooths out when stretched over the screen.
        const Vector2D full = wall.image->m_size;
        const int w = 48, h = std::max(1, (int)std::lround(w * full.y / full.x));
        auto* small = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        auto* cr = cairo_create(small);
        cairo_scale(cr, w / full.x, h / full.y);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_flush(small);
        wall.blurred = g_pHyprRenderer->createTexture(small);
        cairo_surface_destroy(small);
    }

    void render() {
        for (const int id : dirty) load(id);
        dirty.clear();
        if (walls.empty()) return;

        const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!monitor) return;
        for (const auto& ref : State::workspaceState()->workspaceRefs()) {
            const auto ws = ref.lock();
            if (!ws || ws->m_monitor != monitor) continue;
            const auto it = walls.find(ws->m_id);
            if (it == walls.end()) continue;
            const auto& wall = it->second;
            if (ws != monitor->m_activeWorkspace && !ws->m_forceRendering && !ws->m_renderOffset->isBeingAnimated() && !ws->m_alpha->isBeingAnimated())
                continue;

            // Fit the whole image over misc:background_color, optionally under a dimmed blurred fill.
            static auto bgColor = CConfigValue<Config::INTEGER>("misc:background_color");
            const Vector2D origin = ws->m_renderOffset->value() * monitor->m_scale;
            const Vector2D size = monitor->m_transformedSize;
            const float alpha = ws->m_alpha->value();
            CHyprColor color(*bgColor);
            color.a *= alpha;
            g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = {origin, size}, .color = color}));
            if (cfgBlurFill()) drawImage(wall.blurred, origin, size, alpha * 0.6F, true);
            drawImage(wall.image, origin, size, alpha, false);
        }
    }

    // Scale tex to fit (or cover) the box at origin/size, centered and clipped to it.
    static void drawImage(const SP<Render::ITexture>& tex, const Vector2D& origin, const Vector2D& size, float alpha, bool cover) {
        const double scale = cover ? std::max(size.x / tex->m_size.x, size.y / tex->m_size.y) : std::min(size.x / tex->m_size.x, size.y / tex->m_size.y);
        CTexPassElement::SRenderData data;
        data.tex = tex;
        data.box = {origin + (size - tex->m_size * scale) / 2.0, tex->m_size * scale};
        data.clipBox = {origin, size};
        data.a = alpha;
        g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(std::move(data)));
    }
} wallpapers;

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;
    blurFill = makeShared<Config::Values::CBoolValue>("plugin:hyprmosaic:blur_fill", "Fill around wallpapers with a blurred copy instead of misc:background_color", true);
    if (!HyprlandAPI::addConfigValueV2(PHANDLE, blurFill)) blurFill.reset();
    columnsCfg = makeShared<Config::Values::CIntValue>("plugin:hyprmosaic:columns", "Grid columns (workspaces 1..columns*rows, row by row)", 3, Config::Values::SIntValueOptions{.min = 1, .max = 10});
    if (!HyprlandAPI::addConfigValueV2(PHANDLE, columnsCfg)) columnsCfg.reset();
    rowsCfg = makeShared<Config::Values::CIntValue>("plugin:hyprmosaic:rows", "Grid rows; 1 for a single row of workspaces", 3, Config::Values::SIntValueOptions{.min = 1, .max = 10});
    if (!HyprlandAPI::addConfigValueV2(PHANDLE, rowsCfg)) rowsCfg.reset();
    fingersCfg = makeShared<Config::Values::CIntValue>("plugin:hyprmosaic:fingers", "Fingers for grid swipes; 0 disables them (wallpapers only)", 3, Config::Values::SIntValueOptions{.min = 0, .max = 5});
    if (!HyprlandAPI::addConfigValueV2(PHANDLE, fingersCfg)) fingersCfg.reset();

    beginListener = Event::bus()->m_events.gesture.swipe.begin.listen(
        [](IPointer::SSwipeBeginEvent e, Event::SCallbackInfo& info) {
            if (cfgFingers() > 0 && (int)e.fingers == cfgFingers()) { info.cancelled = true; swipe.begin(); }
        });
    updateListener = Event::bus()->m_events.gesture.swipe.update.listen(
        [](IPointer::SSwipeUpdateEvent e, Event::SCallbackInfo& info) {
            if (swipe.tracking) { info.cancelled = true; swipe.update(e); }
        });
    endListener = Event::bus()->m_events.gesture.swipe.end.listen(
        [](IPointer::SSwipeEndEvent e, Event::SCallbackInfo& info) {
            if (swipe.tracking) { info.cancelled = true; swipe.end(e.cancelled); }
        });
    renderListener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage == RENDER_POST_WALLPAPER) wallpapers.render();
    });
    wallpapers.init();
    return {"hyprmosaic", "Per-workspace wallpapers that slide with their workspace, plus grid workspace swipes", "La5u", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    beginListener.reset();
    updateListener.reset();
    endListener.reset();
    renderListener.reset();
    wallpapers.exit();
    blurFill.reset();
    columnsCfg.reset();
    rowsCfg.reset();
    fingersCfg.reset();
}
