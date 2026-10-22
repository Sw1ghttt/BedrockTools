#include "waypoints.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>

static WaypointsModule* g_wpMod = nullptr;

static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static float wrap180(float a) {
    a = std::fmod(a + 180.f, 360.f);
    if (a < 0.f) a += 360.f;
    return a - 180.f;
}

static unsigned parseColor(const std::string& s, unsigned fallback) {
    if (s.size() < 2 || s[0] != '#') return fallback;
    char* end = nullptr;
    unsigned long v = std::strtoul(s.c_str() + 1, &end, 16);
    if (end == s.c_str() + 1) return fallback;
    if (s.size() == 7) return 0xFF000000u | (unsigned)v;
    if (s.size() == 9) return (unsigned)v; // #AARRGGBB
    return fallback;
}

static std::string distText(double d) {
    char b[24];
    if (d >= 1000.0) std::snprintf(b, sizeof b, "%.1fkm", d / 1000.0);
    else std::snprintf(b, sizeof b, "%dm", (int)std::lround(d));
    return b;
}

static uint32_t withAlpha(uint32_t argb, float a) {
    uint32_t al = (uint32_t)(std::max(0.f, std::min(1.f, a)) * (float)((argb >> 24) & 0xFF));
    return (al << 24) | (argb & 0x00FFFFFFu);
}

WaypointsModule::WaypointsModule()
    : Module("Waypoints", "Save locations and see their direction and distance on a HUD bar.") {
    g_wpMod = this;
}

WaypointsModule::~WaypointsModule() {
    if (g_wpMod == this) g_wpMod = nullptr;
}

void WaypointsModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        if (g_wpMod && g_wpMod->enabled) g_wpMod->onTick(event.player);
    });
}

void WaypointsModule::onEnable() {}
void WaypointsModule::onDisable() { m_valid = false; }

void WaypointsModule::onTick(bedrocktools::sdk::Player* player) {
    if (!player) { m_valid = false; return; }
    m_pos = player->position();
    m_yaw = player->rotation().y;
    int dim = 0;
    if (auto* d = player->dimension()) {
        if (auto* bs = d->blockSource()) dim = bs->dimensionId();
    }
    m_dim = (dim >= 0 && dim <= 2) ? dim : 0;
    m_valid = true;
}

void WaypointsModule::notify(const std::string& msg) {
    m_toast = msg;
    m_toastUntil = nowSec() + 3.0;
}

void WaypointsModule::addHere() {
    if (!m_valid) { notify("Waypoints: join a world first"); return; }
    WaypointEntry w;
    w.x = (int)std::floor(m_pos.x);
    w.y = (int)std::floor(m_pos.y);
    w.z = (int)std::floor(m_pos.z);
    w.dim = m_dim;
    std::string nm = m_waypointName;
    size_t b0 = nm.find_first_not_of(" \t");
    size_t b1 = nm.find_last_not_of(" \t");
    nm = (b0 == std::string::npos) ? std::string() : nm.substr(b0, b1 - b0 + 1);
    w.name = nm.empty() ? ("Waypoint " + std::to_string(m_points.size() + 1)) : nm;
    w.color = parseColor(m_markerColor, 0xFF55FF55u);
    m_points.push_back(w);
    notify("Added: " + w.name);
}

bool WaypointsModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "addHereButton") { addHere(); return true; }
    if (key == "deleteSelectedButton") {
        int i = m_deleteNumber - 1;
        if (i >= 0 && i < (int)m_points.size()) {
            notify("Deleted: " + m_points[i].name);
            m_points.erase(m_points.begin() + i);
        } else notify("No waypoint with that number");
        return true;
    }
    if (key == "toggleSelectedButton") {
        int i = m_deleteNumber - 1;
        if (i >= 0 && i < (int)m_points.size()) {
            m_points[i].visible = !m_points[i].visible;
            notify(m_points[i].name + (m_points[i].visible ? ": shown" : ": hidden"));
        } else notify("No waypoint with that number");
        return true;
    }
    if (key == "clearAllButton") {
        // two-step safety: only clears when the selected number is 0
        if (m_deleteNumber == 0) { m_points.clear(); notify("All waypoints cleared"); }
        else notify("Set Delete Number to 0, then press Clear All");
        return true;
    }
    return false; // everything else handled by the generic config path
}

void WaypointsModule::onFrame() {
    if (!enabled) return;
    std::vector<PLModMenu_DrawCommand> cmds;

    const float sc = m_scale;
    const float barW = m_barWidth * sc;
    const float barH = 40.f * sc;
    const float barX = hudPosX < 0.f ? 100.f : hudPosX;
    const float barY = hudPosY < 0.f ? 100.f : hudPosY;
    const float cx = barX + barW * 0.5f;
    const float midY = barY + barH * 0.5f;
    const float range = std::max(10.f, std::min(180.f, m_range));
    const float pxPerDeg = (barW * 0.5f) / range;
    const float fs = 11.f * sc * 2.5f;

    auto text = [&](float x, float y, const std::string& t, uint32_t col, float size) {
        PLModMenu_DrawCommand c = {};
        c.type = PL_DRAW_TEXT; c.x = x; c.y = y; c.w = 1.f; c.h = 1.f;
        c.color = col; c.size = size; c.text = t.c_str();
        cmds.push_back(c);
    };
    // NOTE: draw commands keep a pointer to text, so keep strings alive until submit.
    std::deque<std::string> keep;
    auto textK = [&](float x, float y, std::string t, uint32_t col, float size) {
        keep.push_back(std::move(t));
        text(x, y, keep.back(), col, size);
    };

    if (m_showBar) {
        PLModMenu_DrawCommand bg = {};
        bg.type = PL_DRAW_RECT_FILLED; bg.x = barX; bg.y = barY; bg.w = barW; bg.h = barH;
        bg.color = (uint32_t)(m_opacity * 140) << 24;
        cmds.push_back(bg);

        PLModMenu_DrawCommand center = {};
        center.type = PL_DRAW_LINE; center.x = cx; center.y = barY; center.w = 0; center.h = barH;
        center.color = withAlpha(0xFFFFFFFFu, m_opacity * 0.5f); center.size = 1.f * sc;
        cmds.push_back(center);
    }

    struct Row { int num; const WaypointEntry* w; double dist; float delta; };
    std::vector<Row> rows;

    if (m_valid) {
        for (size_t i = 0; i < m_points.size(); ++i) {
            const auto& w = m_points[i];
            if (!w.visible) continue;
            if (m_currentDimensionOnly && w.dim != m_dim) continue;
            double px = w.x + 0.5, pz = w.z + 0.5, py = w.y;
            if (m_netherScaling && w.dim != m_dim) {
                if (w.dim == 0 && m_dim == 1) { px /= 8.0; pz /= 8.0; }
                else if (w.dim == 1 && m_dim == 0) { px *= 8.0; pz *= 8.0; }
            }
            double dx = px - m_pos.x, dy = py - m_pos.y, dz = pz - m_pos.z;
            double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (m_maxDistanceKm > 0.f && dist > (double)m_maxDistanceKm * 1000.0) continue;
            if (dist < m_hideCloserBlocks) continue;
            // Minecraft yaw: 0 = +Z (south), 90 = -X (west)
            float yawTo = (float)(std::atan2(-dx, dz) * 180.0 / M_PI);
            rows.push_back({(int)i + 1, &w, dist, wrap180(yawTo - m_yaw)});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.dist < b.dist; });
    }

    if (m_showBar) {
        for (const auto& r : rows) {
            float d = r.delta;
            bool off = std::fabs(d) > range;
            if (off && !m_showOffscreen) continue;
            float cd = std::max(-range, std::min(range, d));
            float sx = cx + cd * pxPerDeg;
            uint32_t col = withAlpha(r.w->color, m_opacity);
            float r0 = 6.f * sc;
            PLModMenu_DrawCommand dot = {};
            if (off) {
                float dirn = d > 0 ? 1.f : -1.f; // points to the side the waypoint is on
                dot.type = PL_DRAW_TRIANGLE_FILLED;
                dot.x = sx - dirn * r0; dot.y = midY - r0;
                dot.w = sx - dirn * r0; dot.h = midY + r0;
                dot.x3 = sx + dirn * r0; dot.y3 = midY;
            } else {
                dot.type = PL_DRAW_CIRCLE_FILLED;
                dot.x = sx; dot.y = midY; dot.size = r0;
            }
            dot.color = col;
            cmds.push_back(dot);
            if (!off && m_showNames) {
                std::string label = r.w->name;
                textK(sx, barY - 14.f * sc, label, col, 11.f * sc * 2.5f);
            }
            if (!off && m_showDistance) textK(sx, barY + barH + 3.f * sc, distText(r.dist), withAlpha(0xFFDDDDDDu, m_opacity), 9.f * sc * 2.5f);
        }
    }

    float ly = barY + barH + 22.f * sc;
    if (m_showCoords && m_valid) {
        char b[64];
        std::snprintf(b, sizeof b, "XYZ %d %d %d", (int)std::floor(m_pos.x), (int)std::floor(m_pos.y), (int)std::floor(m_pos.z));
        textK(barX, ly, b, 0xFFFFFFFFu, fs); ly += fs + 2.f;
    }
    if (m_showList) {
        int n = std::max(1, std::min(10, m_listSize));
        int shown = 0;
        for (const auto& r : rows) {
            if (shown++ >= n) break;
            const char* arrow = std::fabs(r.delta) < 15.f ? "^" : (r.delta > 0 ? (r.delta > 90.f ? ">>" : ">") : (r.delta < -90.f ? "<<" : "<"));
            char b[160];
            std::snprintf(b, sizeof b, "%d. %s  %s  %s", r.num, r.w->name.c_str(), distText(r.dist).c_str(), arrow);
            textK(barX, ly, b, withAlpha(r.w->color, 1.f), fs); ly += fs + 2.f;
        }
    }
    if (!m_toast.empty()) {
        if (nowSec() < m_toastUntil) textK(barX, ly + 4.f, m_toast, 0xFFFFFF55u, fs);
        else m_toast.clear();
    }

    submitDrawCommands(moduleId, cmds);
}

void WaypointsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    auto f = [&](const char* k, auto& v) { if (j.contains(k)) { try { v = j[k].get<std::decay_t<decltype(v)>>(); } catch (...) {} } };
    f("hudPosX", hudPosX); f("hudPosY", hudPosY); f("isHudModule", isHudModule);
    f("m_waypointName", m_waypointName);
    f("m_showBar", m_showBar); f("m_showNames", m_showNames); f("m_showDistance", m_showDistance);
    f("m_showList", m_showList); f("m_listSize", m_listSize); f("m_showOffscreen", m_showOffscreen);
    f("m_netherScaling", m_netherScaling); f("m_currentDimensionOnly", m_currentDimensionOnly);
    f("m_showCoords", m_showCoords); f("m_barWidth", m_barWidth); f("m_scale", m_scale);
    f("m_range", m_range); f("m_opacity", m_opacity); f("m_maxDistanceKm", m_maxDistanceKm);
    f("m_hideCloserBlocks", m_hideCloserBlocks); f("m_deleteNumber", m_deleteNumber);
    f("m_markerColor", m_markerColor);
    m_listSize = std::max(1, std::min(10, m_listSize));
    m_scale = std::max(0.3f, std::min(5.f, m_scale));
    m_opacity = std::max(0.f, std::min(1.f, m_opacity));
    if (j.contains("points") && j["points"].is_array()) {
        std::vector<WaypointEntry> out;
        for (auto& p : j["points"]) {
            try {
                WaypointEntry w;
                w.name = p.value("name", std::string("Waypoint"));
                w.x = p.value("x", 0); w.y = p.value("y", 64); w.z = p.value("z", 0);
                w.dim = p.value("dim", 0);
                w.color = p.value("color", 0xFF55FF55u);
                w.visible = p.value("visible", true);
                out.push_back(w);
            } catch (...) {}
        }
        m_points = std::move(out);
    }
}

void WaypointsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX; j["hudPosY"] = hudPosY; j["isHudModule"] = isHudModule;
    // action buttons (boolean keys containing "button" become buttons in the menu)
    j["addHereButton"] = false;
    j["m_waypointName"] = m_waypointName.empty() ? std::string(" ") : m_waypointName;
    j["m_markerColor"] = m_markerColor;
    j["m_showBar"] = m_showBar; j["m_showNames"] = m_showNames; j["m_showDistance"] = m_showDistance;
    j["m_showOffscreen"] = m_showOffscreen;
    j["m_showList"] = m_showList; j["m_listSize"] = m_listSize;
    j["m_showCoords"] = m_showCoords;
    j["m_netherScaling"] = m_netherScaling; j["m_currentDimensionOnly"] = m_currentDimensionOnly;
    j["m_barWidth"] = m_barWidth; j["m_scale"] = m_scale; j["m_range"] = m_range; j["m_opacity"] = m_opacity;
    j["m_maxDistanceKm"] = m_maxDistanceKm; j["m_hideCloserBlocks"] = m_hideCloserBlocks;
    j["m_deleteNumber"] = m_deleteNumber;
    j["toggleSelectedButton"] = false;
    j["deleteSelectedButton"] = false;
    j["clearAllButton"] = false;
    nlohmann::json arr = nlohmann::json::array();
    for (auto& w : m_points)
        arr.push_back({{"name", w.name}, {"x", w.x}, {"y", w.y}, {"z", w.z}, {"dim", w.dim}, {"color", w.color}, {"visible", w.visible}});
    j["points"] = arr;
}
