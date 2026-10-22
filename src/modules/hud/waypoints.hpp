#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <string>
#include <vector>

struct WaypointEntry {
    std::string name;
    int x = 0, y = 64, z = 0;
    int dim = 0; // 0 overworld, 1 nether, 2 end
    unsigned color = 0xFF55FF55;
    bool visible = true;
};

class WaypointsModule : public Module {
public:
    WaypointsModule();
    ~WaypointsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void onTick(bedrocktools::sdk::Player* player);

private:
    void addHere();
    void notify(const std::string& msg);

    // live player state
    bool m_valid = false;
    bedrocktools::sdk::Vec3 m_pos{0, 0, 0};
    float m_yaw = 0.f;
    int m_dim = 0;

    std::vector<WaypointEntry> m_points;
    std::string m_toast;
    double m_toastUntil = 0.0;

    // HUD placement (draggable in the HUD editor)
    float hudPosX = 400.f;
    float hudPosY = 50.f;
    bool isHudModule = true;

    // settings (each one shows up in the Mod Menu automatically)
    std::string m_waypointName = "";
    bool m_showBar = true;
    bool m_showNames = true;
    bool m_showDistance = true;
    bool m_showList = true;
    int m_listSize = 5;             // clamped 1..10
    bool m_showOffscreen = true;
    bool m_netherScaling = true;
    bool m_currentDimensionOnly = true;
    bool m_showCoords = false;
    float m_barWidth = 400.f;
    float m_scale = 1.0f;
    float m_range = 90.0f;          // degrees either side of center
    float m_opacity = 0.8f;
    float m_maxDistanceKm = 0.f;    // 0 = unlimited
    float m_hideCloserBlocks = 3.f; // hide markers closer than this
    int m_deleteNumber = 1;         // list number used by "Delete Selected"
    std::string m_markerColor = "#55FF55"; // color for NEW waypoints
};
