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
#include <libinput.h>

#include <hyprgraphics/image/Image.hpp>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/Compositor.hpp>

// Everything but the plugin entry points has internal linkage, so a reloaded copy never calls into an old one.
namespace {

static HANDLE PHANDLE = nullptr;
static Hyprutils::Signal::CHyprSignalListener beginListener, updateListener, endListener, renderListener, tickListener, reloadListener;
static SP<Config::Values::CBoolValue> blurFill, wrapCfg, specialSwipeCfg, freeSwipeCfg, quietLibinputCfg;
static SP<Config::Values::CIntValue> columnsCfg, rowsCfg, fingersCfg;

// A workspace's number, or 0 for special workspaces.
static int numberedID(const PHLWORKSPACE& ws) {
    const auto id = ws->id();
    const auto numbered = std::get_if<Workspace::SWorkspaceNumberedID>(&id);
    return numbered ? (int)numbered->value : 0;
}

// Config values, with defaults in case registration failed.
static bool cfgBlurFill() { return !blurFill || blurFill->value(); }
static bool cfgSpecialSwipe() { return specialSwipeCfg && specialSwipeCfg->value(); }
static bool cfgFreeSwipe() { return freeSwipeCfg && freeSwipeCfg->value(); }
static bool cfgQuietLibinput() { return quietLibinputCfg && quietLibinputCfg->value(); }
static bool cfgWrap() { return wrapCfg && wrapCfg->value(); }
static int cfgColumns() { return columnsCfg ? std::clamp<int>(columnsCfg->value(), 1, 10) : 3; }
static int cfgRows() { return rowsCfg ? std::clamp<int>(rowsCfg->value(), 1, 10) : 3; }
static int cfgFingers() { return fingersCfg ? fingersCfg->value() : 3; }

// Aquamarine asks libinput for debug messages and logs them all, whatever debug:disable_logs says.
// With quiet_libinput, lower that to errors; otherwise (and on unload) put back aquamarine's level.
static void applyLibinputLogLevel(bool quiet) {
    const auto& session = g_pCompositor->m_aqBackend ? g_pCompositor->m_aqBackend->session : nullptr;
    if (!session || !session->libinputHandle) return;
    libinput_log_set_priority(session->libinputHandle, quiet ? LIBINPUT_LOG_PRIORITY_ERROR : LIBINPUT_LOG_PRIORITY_DEBUG);
}

struct GridSwipe {
    // A workspace drawn next to the start during a swipe, `x`/`y` cells away (each -1..1).
    struct Neighbour {
        int x = 0, y = 0;
        PHLWORKSPACE ws;
    };

    bool tracking = false;
    bool active = false;
    bool freeAxes = false;  // both axes at once, reaching diagonal neighbours
    bool vertical = false;  // the locked axis when not free
    int columns = 3, rows = 3; // grid size, fixed for the duration of a swipe
    bool wrap = false;          // swiping past an edge lands on the opposite one
    Vector2D total{};
    Vector2D delta{};
    Vector2D averageSpeed{};
    int speedPoints = 0;
    PHLWORKSPACE start;
    // Hyprland only keeps a workspace alive while it is shown or has windows. Hold the neighbours ourselves
    // during the swipe, and afterwards until their release animation ends, or empty ones vanish mid-animation.
    std::vector<Neighbour> shown;
    // With special_swipe, a special workspace open over the start slides out with it, and closes if the swipe commits.
    PHLWORKSPACE special;
    float specialFade = 0, specialDim = 0, specialBlur = 0;
    std::vector<PHLWORKSPACE> settling;
    UP<SEventLoopDoLaterLock> releaseLater;
    PHLMONITORREF monitor;

    static int sign(double v) { return (v > 0) - (v < 0); }

    // Workspaces 1..columns*rows, laid out row by row.
    bool valid(int id) const { return id >= 1 && id <= columns * rows; }
    // Workspace `x` columns and `y` rows from the start, or 0 past an edge unless wrapping.
    int cellID(int x, int y) const {
        const int id = numberedID(start) - 1;
        int col = id % columns + x, row = id / columns + y;
        if (col < 0 || col >= columns) {
            if (!wrap) return 0;
            col = (col + columns) % columns;
        }
        if (row < 0 || row >= rows) {
            if (!wrap) return 0;
            row = (row + rows) % rows;
        }
        return row * columns + col + 1;
    }
    PHLWORKSPACE get(int id) const {
        return valid(id) ? State::workspaceState()->query().numbered(Workspace::SWorkspaceNumberedID(id)).run() : nullptr;
    }
    PHLWORKSPACE ensure(int id) {
        if (auto ws = get(id)) return ws;
        return State::workspaceState()->createNumbered(Workspace::SWorkspaceNumberedID(id), monitor.lock());
    }
    // One cell's step on screen, per axis.
    Vector2D cellSize() const {
        static auto gap = CConfigValue<Config::INTEGER>("general:gaps_workspaces");
        return monitor->m_size + Vector2D{(double)*gap, (double)*gap};
    }
    Vector2D cellOffset(int x, int y) const {
        return cellSize() * Vector2D{(double)x, (double)y};
    }

    // Move the special workspace to `at`, fading its dim/blur by how far the swipe has gone (0..1).
    void slideSpecial(Vector2D at, double progress) {
        if (!special) return;
        const float keep = 1.F - (float)std::clamp(progress, 0.0, 1.0);
        special->m_renderOffset->setValueAndWarp(at);
        monitor->m_specialFade->setValueAndWarp(specialFade * keep);
        monitor->m_specialDim->setValueAndWarp(specialDim * keep);
        monitor->m_specialBlur->setValueAndWarp(specialBlur * keep);
    }

    // A target is reachable if it exists in the grid, isn't the start and doesn't live on another monitor.
    bool reachable(int id) const {
        if (!valid(id) || id == numberedID(start)) return false;
        const auto ws = get(id);
        return !ws || ws->m_monitor == monitor;
    }

    void begin() {
        const auto focused = Desktop::focusState()->monitor();
        if (!focused || !focused->m_activeWorkspace) return;
        tracking = true;
        columns = cfgColumns();
        rows = cfgRows();
        wrap = cfgWrap();
        freeAxes = cfgFreeSwipe();
        active = false;
        total = {};
        delta = {};
        averageSpeed = {};
        speedPoints = 0;
        shown.clear();
        start = focused->m_activeWorkspace;
        monitor = focused;
        special = cfgSpecialSwipe() ? focused->m_activeSpecialWorkspace : nullptr;
        if (special) {
            specialFade = focused->m_specialFade->value();
            specialDim = focused->m_specialDim->value();
            specialBlur = focused->m_specialBlur->value();
        }
    }

    void update(const IPointer::SSwipeUpdateEvent& e) {
        if (!tracking || !start || !monitor) return;
        total += e.delta;

        // Lock to the dominant physical axis once enough motion is available, unless swiping freely.
        if (!active) {
            if (std::abs(total.x) < 5 && std::abs(total.y) < 5) return;
            vertical = std::abs(total.y) > std::abs(total.x);
            active = valid(numberedID(start));
            if (!active) return;
        }

        static auto distanceCfg = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_distance");
        static auto invert = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_invert");
        const double distance = std::max<int64_t>(1, *distanceCfg);
        Vector2D movement = *invert ? -e.delta : e.delta;
        if (!freeAxes) movement = vertical ? Vector2D{0.0, movement.y} : Vector2D{movement.x, 0.0};
        const Vector2D old = delta;
        delta += movement;
        delta = {std::clamp(delta.x, -distance, distance), std::clamp(delta.y, -distance, distance)};
        ++speedPoints;
        const Vector2D moved = delta - old;
        averageSpeed = (averageSpeed * (speedPoints - 1) + Vector2D{std::abs(moved.x), std::abs(moved.y)}) / speedPoints;

        // Drop an axis whose neighbour can't be reached; for a diagonal on another monitor, keep the stronger axis.
        int x = sign(delta.x), y = sign(delta.y);
        if (x && !reachable(cellID(x, 0))) delta.x = x = 0;
        if (y && !reachable(cellID(0, y))) delta.y = y = 0;
        if (x && y && !reachable(cellID(x, y))) {
            if (std::abs(delta.x) >= std::abs(delta.y)) delta.y = y = 0;
            else delta.x = x = 0;
        }

        std::vector<Neighbour> next;
        if (x) next.push_back({x, 0, ensure(cellID(x, 0))});
        if (y) next.push_back({0, y, ensure(cellID(0, y))});
        if (x && y) next.push_back({x, y, ensure(cellID(x, y))});

        // Hide every other neighbour on this monitor: ones the swipe moved away from, and ones still sliding out
        // from an earlier switch.
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const auto ws = get(cellID(dx, dy));
                if (!ws || ws == start || ws->m_monitor != monitor || std::ranges::any_of(next, [&](const auto& m) { return m.ws == ws; })) continue;
                ws->m_forceRendering = false;
                ws->m_alpha->setValueAndWarp(0.F);
            }
        shown = std::move(next);

        const Vector2D base = -(delta / distance) * cellSize();
        start->m_forceRendering = !shown.empty();
        start->m_renderOffset->setValueAndWarp(base);
        start->updateWindowDecos();
        for (const auto& n : shown) {
            n.ws->m_forceRendering = true;
            n.ws->m_alpha->setValueAndWarp(1.F);
            n.ws->m_renderOffset->setValueAndWarp(base + cellOffset(n.x, n.y));
            n.ws->updateWindowDecos();
        }
        slideSpecial(base, std::max(std::abs(delta.x), std::abs(delta.y)) / distance);
        g_pHyprRenderer->damageMonitor(monitor.lock());
    }

    void end(bool cancelled) {
        if (!tracking) return;
        tracking = false;
        if (!active || !start || !monitor) {
            shown.clear();
            special = nullptr;
            active = false;
            return;
        }

        static auto ratio = CConfigValue<Config::FLOAT>("gestures:workspace_swipe_cancel_ratio");
        static auto distanceCfg = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_distance");
        static auto force = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_min_speed_to_force");
        const double distance = std::max<int64_t>(1, *distanceCfg);
        // Each axis commits on its own, so a free swipe lands on whichever cell it went far or fast enough towards.
        const auto commits = [&](double d, double speed) {
            return std::abs(d) >= 2 && (std::abs(d) >= distance * *ratio || (*force != 0 && speed >= *force));
        };
        const int x = commits(delta.x, averageSpeed.x) ? sign(delta.x) : 0;
        const int y = commits(delta.y, averageSpeed.y) ? sign(delta.y) : 0;
        const int targetID = cellID(x, y);
        const bool cancel = cancelled || !reachable(targetID);

        if (cancel) {
            for (const auto& n : shown) *n.ws->m_renderOffset = cellOffset(n.x, n.y);
            *start->m_renderOffset = Vector2D{};
            if (special) {
                *special->m_renderOffset = Vector2D{};
                *monitor->m_specialFade = specialFade;
                *monitor->m_specialDim = specialDim;
                *monitor->m_specialBlur = specialBlur;
            }
        } else {
            // changeWorkspace() restarts both workspaces' animations, so continue them from where the swipe left them.
            auto target = ensure(targetID);
            const auto targetOffset = target->m_renderOffset->value();
            const auto startOffset = start->m_renderOffset->value();
            if (special && monitor->m_activeSpecialWorkspace == special) {
                monitor->setSpecialWorkspace(nullptr);
                // Hidden now; put it back in place for the next time it opens.
                special->m_renderOffset->setValueAndWarp({});
            }
            monitor->changeWorkspace(target);
            target->m_renderOffset->setValue(targetOffset);
            target->m_alpha->setValueAndWarp(1.F);
            start->m_renderOffset->setValue(startOffset);
            *start->m_renderOffset = cellOffset(-x, -y);
            start->m_alpha->setValueAndWarp(1.F);
            // The other neighbours slide on to their place relative to the target.
            for (const auto& n : shown)
                if (n.ws != target) *n.ws->m_renderOffset = cellOffset(n.x - x, n.y - y);
            g_pInputManager->unconstrainMouse();
        }

        if (std::ranges::find(settling, start) == settling.end()) settling.push_back(start);
        for (const auto& n : shown) {
            n.ws->m_forceRendering = false;
            if (std::ranges::find(settling, n.ws) == settling.end()) settling.push_back(n.ws);
        }
        start->m_forceRendering = false;
        g_pHyprRenderer->damageMonitor(monitor.lock());
        g_pInputManager->refocus();
        start = nullptr;
        shown.clear();
        special = nullptr;
        active = false;
    }

    // Called every frame: once the settling workspaces stop animating, let them go (outside rendering).
    void releaseSettled() {
        if (settling.empty() || releaseLater) return;
        if (std::ranges::any_of(settling, [](const auto& ws) { return ws->m_renderOffset->isBeingAnimated() || ws->m_alpha->isBeingAnimated(); }))
            return;
        releaseLater = g_pEventLoopManager->doLaterLock([this] {
            settling.clear();
            releaseLater.reset();
        });
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

    void render(Render::CRenderContext& ctx) {
        swipe.releaseSettled();
        for (const int id : dirty) load(id);
        dirty.clear();
        if (walls.empty()) return;

        const auto monitor = ctx.m_data.pMonitor.lock();
        if (!monitor) return;
        for (const auto& ref : State::workspaceState()->workspaceRefs()) {
            const auto ws = ref.lock();
            if (!ws || ws->m_monitor != monitor) continue;
            const auto it = walls.find(numberedID(ws));
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
            Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = {origin, size}, .color = color}));
            if (cfgBlurFill()) drawImage(ctx, wall.blurred, origin, size, alpha * 0.6F, true);
            drawImage(ctx, wall.image, origin, size, alpha, false);
        }
    }

    // Hyprland only redraws a workspace's windows while it animates, never the background, so
    // redraw monitors where a workspace with a wallpaper is moving. Runs on every animation tick.
    void damageAnimating() {
        for (const auto& ref : State::workspaceState()->workspaceRefs()) {
            const auto ws = ref.lock();
            if (!ws || !walls.contains(numberedID(ws)) || (!ws->m_renderOffset->isBeingAnimated() && !ws->m_alpha->isBeingAnimated())) continue;
            if (const auto monitor = ws->m_monitor.lock()) g_pHyprRenderer->damageMonitor(monitor);
        }
    }

    // Scale tex to fit (or cover) the box at origin/size, centered and clipped to it.
    static void drawImage(Render::CRenderContext& ctx, const SP<Render::ITexture>& tex, const Vector2D& origin, const Vector2D& size, float alpha, bool cover) {
        const double scale = cover ? std::max(size.x / tex->m_size.x, size.y / tex->m_size.y) : std::min(size.x / tex->m_size.x, size.y / tex->m_size.y);
        CTexPassElement::SRenderData data;
        data.tex = tex;
        data.box = {origin + (size - tex->m_size * scale) / 2.0, tex->m_size * scale};
        data.clipBox = {origin, size};
        data.a = alpha;
        Render::IHyprRenderer::addPassElement(ctx, makeUnique<CTexPassElement>(std::move(data)));
    }
} wallpapers;

} // namespace

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
    wrapCfg = makeShared<Config::Values::CBoolValue>("plugin:hyprmosaic:wrap", "Swiping past a grid edge wraps around to the opposite edge", false);
    if (!HyprlandAPI::addConfigValueV2(PHANDLE, wrapCfg)) wrapCfg.reset();
    specialSwipeCfg = makeShared<Config::Values::CBoolValue>("plugin:hyprmosaic:special_swipe", "Grid swipes slide an open special workspace out and close it", false);
    if (!HyprlandAPI::addConfigValueV2(PHANDLE, specialSwipeCfg)) specialSwipeCfg.reset();
    freeSwipeCfg = makeShared<Config::Values::CBoolValue>("plugin:hyprmosaic:free_swipe", "Grid swipes move along both axes at once, reaching diagonal workspaces", false);
    if (!HyprlandAPI::addConfigValueV2(PHANDLE, freeSwipeCfg)) freeSwipeCfg.reset();
    quietLibinputCfg = makeShared<Config::Values::CBoolValue>("plugin:hyprmosaic:quiet_libinput", "Only log libinput errors, not its per-event debug messages", false);
    if (!HyprlandAPI::addConfigValueV2(PHANDLE, quietLibinputCfg)) quietLibinputCfg.reset();

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
    renderListener = Event::bus()->m_events.render.stage.listen([](const Event::SRenderStageEvent& e) {
        if (e.stage == RENDER_POST_WALLPAPER && e.context) wallpapers.render(e.context->get());
    });
    tickListener = Event::bus()->m_events.tick.listen([] { wallpapers.damageAnimating(); });
    reloadListener = Event::bus()->m_events.config.reloaded.listen([] { applyLibinputLogLevel(cfgQuietLibinput()); });
    applyLibinputLogLevel(cfgQuietLibinput());
    wallpapers.init();
    return {"hyprmosaic", "Per-workspace wallpapers that slide with their workspace, plus grid workspace swipes", "La5u", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    beginListener.reset();
    updateListener.reset();
    endListener.reset();
    renderListener.reset();
    tickListener.reset();
    reloadListener.reset();
    applyLibinputLogLevel(false);
    wallpapers.exit();
    swipe.releaseLater.reset();
    swipe.settling.clear();
    swipe.shown.clear();
    swipe.start.reset();
    swipe.special.reset();
    blurFill.reset();
    wrapCfg.reset();
    specialSwipeCfg.reset();
    freeSwipeCfg.reset();
    quietLibinputCfg.reset();
    columnsCfg.reset();
    rowsCfg.reset();
    fingersCfg.reset();
}
