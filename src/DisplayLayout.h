#pragma once

#include "Models.h"

#include <algorithm>
#include <vector>
#include <windows.h>

// Windows assigns new adapter ids (LUIDs) after a restart or driver update, so a
// monitor is also the same when its target id and name match.
inline bool IsSameMonitor(const MonitorPowerSetup::DisplayPath& left, const MonitorPowerSetup::DisplayPath& right)
{
    if (left.targetId != right.targetId) return false;
    if (left.targetAdapterLowPart == right.targetAdapterLowPart && left.targetAdapterHighPart == right.targetAdapterHighPart)
        return true;
    return !left.monitorName.empty() && left.monitorName == right.monitorName;
}

// Positions captured at different times can come from different desktops: a monitor
// that was the only active one sits at 0,0 just like the first monitor of another
// arrangement. Windows needs monitors that do not overlap, so overlapping monitors
// move right, keeping their left-to-right order; later ones move along by the same
// amount. A layout without overlaps stays unchanged. Heights are not stored and are
// assumed to be 16:9, as in the Displays page.
inline void SeparateOverlappingDisplays(std::vector<MonitorPowerSetup::DisplayPath>& displays, bool enabledOnly = false)
{
    const auto width = [](const MonitorPowerSetup::DisplayPath& display)
    {
        return static_cast<LONG>(display.width == 0 ? 1920u : display.width);
    };
    std::vector<size_t> order;
    for (size_t index = 0; index < displays.size(); ++index)
        if (!enabledOnly || displays[index].enabled) order.push_back(index);
    // The main monitor keeps its place when two start at the same position.
    std::stable_sort(order.begin(), order.end(), [&displays](size_t left, size_t right)
    {
        const auto& a = displays[left];
        const auto& b = displays[right];
        if (a.positionX != b.positionX) return a.positionX < b.positionX;
        return a.isPrimary && !b.isPrimary;
    });

    std::vector<RECT> placed;
    LONG shift = 0;
    for (const size_t index : order)
    {
        auto& display = displays[index];
        const LONG displayWidth = width(display);
        const LONG height = displayWidth * 9 / 16;
        LONG x = display.positionX + shift;
        for (bool moved = true; moved;)
        {
            moved = false;
            for (const auto& other : placed)
            {
                if (x < other.right && other.left < x + displayWidth &&
                    display.positionY < other.bottom && other.top < display.positionY + height)
                {
                    x = other.right;
                    moved = true;
                }
            }
        }
        shift = x - display.positionX;
        display.positionX = x;
        placed.push_back({x, display.positionY, x + displayWidth, display.positionY + height});
    }
}
