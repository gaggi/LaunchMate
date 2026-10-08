#include "Pages.h"

#include "DisplayLayout.h"
#include "MonitorLayout.h"
#include "MonitorPowerController.h"
#include "ui/PageWindow.h"
#include "ui/RowList.h"

#include <algorithm>
#include <commctrl.h>

namespace
{
    constexpr int kConfigListId = 100;
    constexpr int kNewId = 101;
    constexpr int kNameId = 102;
    constexpr int kHotkeyId = 103;
    constexpr int kLayoutId = 104;
    constexpr int kDetectId = 105;
    constexpr int kApplyId = 106;
    constexpr wchar_t kRemoveGlyph = L'\uE711';

    // Keeps a config's choices for known monitors, takes their current adapter ids and
    // adds monitors the config does not know yet, turned off.
    void MergeDetectedDisplays(MonitorPowerSetup& setup, const std::vector<MonitorPowerSetup::DisplayPath>& detected)
    {
        if (detected.empty()) return;
        std::vector<MonitorPowerSetup::DisplayPath> merged;
        for (const auto& display : detected)
        {
            const auto existing = std::find_if(setup.displayPaths.begin(), setup.displayPaths.end(),
                [&display](const auto& path) { return IsSameMonitor(path, display); });
            if (existing == setup.displayPaths.end())
            {
                auto added = display;
                added.enabled = false;
                added.isPrimary = false;
                merged.push_back(std::move(added));
                continue;
            }
            auto kept = *existing;
            kept.sourceAdapterLowPart = display.sourceAdapterLowPart;
            kept.sourceAdapterHighPart = display.sourceAdapterHighPart;
            kept.sourceId = display.sourceId;
            kept.targetAdapterLowPart = display.targetAdapterLowPart;
            kept.targetAdapterHighPart = display.targetAdapterHighPart;
            kept.displayName = display.displayName;
            merged.push_back(std::move(kept));
        }
        for (const auto& existing : setup.displayPaths)
        {
            if (std::none_of(merged.begin(), merged.end(), [&existing](const auto& path) { return IsSameMonitor(path, existing); }))
                merged.push_back(existing);
        }
        // Monitors detected at different times may share positions; keep the tiles apart.
        SeparateOverlappingDisplays(merged);
        setup.displayPaths = std::move(merged);
    }

    // Empty when the configs can be saved; otherwise what still needs fixing.
    std::wstring Validate(const std::vector<MonitorPowerSetup>& setups)
    {
        std::vector<DWORD> hotkeys;
        for (const auto& setup : setups)
        {
            if (setup.name.empty()) return L"Each config needs a name.";
            if (setup.displayPaths.empty()) return L"Click Detect current to capture the monitors for " + setup.name + L".";
            const auto enabled = std::count_if(setup.displayPaths.begin(), setup.displayPaths.end(), [](const auto& display) { return display.enabled; });
            const auto primary = std::count_if(setup.displayPaths.begin(), setup.displayPaths.end(),
                [](const auto& display) { return display.enabled && display.isPrimary; });
            if (enabled == 0 || primary != 1) return setup.name + L" needs one monitor marked as main monitor (star).";
            if (setup.hotkeyVirtualKey != 0)
            {
                const DWORD hotkey = MAKELONG(setup.hotkeyModifiers, setup.hotkeyVirtualKey);
                if (std::find(hotkeys.begin(), hotkeys.end(), hotkey) != hotkeys.end()) return L"Two configs use the same hotkey.";
                hotkeys.push_back(hotkey);
            }
        }
        return {};
    }

    std::wstring HotkeyText(UINT modifiers, UINT virtualKey)
    {
        if (virtualKey == 0) return L"No hotkey";
        std::wstring text;
        if (modifiers & MOD_CONTROL) text += L"Ctrl+";
        if (modifiers & MOD_ALT) text += L"Alt+";
        if (modifiers & MOD_SHIFT) text += L"Shift+";
        if (modifiers & MOD_WIN) text += L"Win+";
        wchar_t name[64]{};
        const LONG scanCode = static_cast<LONG>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC)) << 16;
        if (GetKeyNameTextW(scanCode, name, static_cast<int>(std::size(name))) > 0) text += name;
        else text += std::to_wstring(virtualKey);
        return text;
    }

    class DisplaysPage : public PageWindow
    {
    public:
        DisplaysPage(const PageContext& context, std::function<bool(size_t)> applySetup)
            : PageWindow(context.headingFont, context.textFont), context_(context), applySetup_(std::move(applySetup)),
              setups_(context.configuration->monitorPowerSetups), detected_(context.configuration->detectedDisplays)
        {
        }

    private:
        void OnCreate() override
        {
            AddButton(kNewId, L"New config");
            configs_.Create(Instance(), Handle(), kConfigListId, HeadingFont(), TextFont());
            configs_.SetEmptyText(L"No configs yet. Arrange your monitors in Windows, then click New config.");
            nameLabel_ = AddLabel(L"Name");
            name_ = AddEdit(kNameId);
            hotkeyLabel_ = AddLabel(L"Hotkey");
            hotkey_ = AddControl(HOTKEY_CLASSW, L"", WS_TABSTOP | WS_BORDER, kHotkeyId, TextFont());
            layout_.Create(Instance(), Handle(), kLayoutId, TextFont());
            hint_ = AddLabel(L"Click a monitor to turn it on or off. The star marks the main monitor.");
            AddButton(kDetectId, L"Detect current");
            AddButton(kApplyId, L"Apply now");
            status_ = AddLabel(L"");
            if (!setups_.empty()) selected_ = 0;
            Load();
        }

        void OnSize(int width, int height) override
        {
            constexpr int listWidth = 220;
            const int x = listWidth + 20;
            const int detailWidth = std::max(300, std::min(width, 900) - x);
            Place(GetDlgItem(Handle(), kNewId), 0, 0, 120, 30);
            Place(configs_.Handle(), 0, 40, listWidth, height - 40);
            const int field = (detailWidth - 16) / 2;
            Place(nameLabel_, x, 0, field, 20);
            Place(name_, x, 22, field, 24);
            Place(hotkeyLabel_, x + field + 16, 0, field, 20);
            Place(hotkey_, x + field + 16, 22, field, 24);
            const int layoutHeight = std::clamp(height - 160, 160, 320);
            Place(layout_.Handle(), x, 62, detailWidth, layoutHeight);
            const int below = 62 + layoutHeight + 8;
            Place(hint_, x, below, detailWidth, 22);
            Place(GetDlgItem(Handle(), kDetectId), x, below + 30, 130, 30);
            Place(GetDlgItem(Handle(), kApplyId), x + 140, below + 30, 110, 30);
            Place(status_, x, below + 66, detailWidth, 22);
        }

        void OnDestroy() override
        {
            // The hotkey control does not report edits, so take them over when leaving.
            Store();
            Commit();
        }

        bool HasSelection() const { return selected_ >= 0 && selected_ < static_cast<int>(setups_.size()); }

        // Reads the name and hotkey fields into the selected config.
        void Store()
        {
            if (!HasSelection() || loading_) return;
            auto& setup = setups_[static_cast<size_t>(selected_)];
            wchar_t name[256]{};
            GetWindowTextW(name_, name, static_cast<int>(std::size(name)));
            setup.name = name;
            const DWORD value = static_cast<DWORD>(SendMessageW(hotkey_, HKM_GETHOTKEY, 0, 0));
            setup.hotkeyVirtualKey = LOBYTE(value);
            const BYTE modifiers = HIBYTE(value);
            setup.hotkeyModifiers = 0;
            if (modifiers & HOTKEYF_CONTROL) setup.hotkeyModifiers |= MOD_CONTROL;
            if (modifiers & HOTKEYF_ALT) setup.hotkeyModifiers |= MOD_ALT;
            if (modifiers & HOTKEYF_SHIFT) setup.hotkeyModifiers |= MOD_SHIFT;
            if (modifiers & HOTKEYF_EXT) setup.hotkeyModifiers |= MOD_WIN;
        }

        // Saves the configs once they are valid and says what is missing otherwise.
        void Commit()
        {
            const auto problem = Validate(setups_);
            if (problem.empty())
            {
                context_.configuration->monitorPowerSetups = setups_;
                context_.configuration->detectedDisplays = detected_;
                context_.scheduleSave();
            }
            SetWindowTextW(status_, problem.empty() ? L"Changes are saved automatically." : (L"Not saved yet: " + problem).c_str());
        }

        // Fills the fields and the layout from the selected config.
        void Load()
        {
            loading_ = true;
            const bool selected = HasSelection();
            for (HWND control : {name_, hotkey_, nameLabel_, hotkeyLabel_, hint_, GetDlgItem(Handle(), kApplyId)})
                EnableWindow(control, selected);
            if (selected)
            {
                auto& setup = setups_[static_cast<size_t>(selected_)];
                MergeDetectedDisplays(setup, detected_);
                SetWindowTextW(name_, setup.name.c_str());
                BYTE modifiers = 0;
                if (setup.hotkeyModifiers & MOD_CONTROL) modifiers |= HOTKEYF_CONTROL;
                if (setup.hotkeyModifiers & MOD_ALT) modifiers |= HOTKEYF_ALT;
                if (setup.hotkeyModifiers & MOD_SHIFT) modifiers |= HOTKEYF_SHIFT;
                if (setup.hotkeyModifiers & MOD_WIN) modifiers |= HOTKEYF_EXT;
                SendMessageW(hotkey_, HKM_SETHOTKEY, MAKEWORD(setup.hotkeyVirtualKey, modifiers), 0);
            }
            else
            {
                SetWindowTextW(name_, L"");
                SendMessageW(hotkey_, HKM_SETHOTKEY, 0, 0);
            }
            loading_ = false;
            RefreshViews();
            const auto problem = Validate(setups_);
            SetWindowTextW(status_, problem.empty() ? L"Changes are saved automatically." : (L"Not saved yet: " + problem).c_str());
        }

        void RefreshViews()
        {
            std::vector<RowList::Row> rows;
            for (size_t index = 0; index < setups_.size(); ++index)
            {
                RowList::Row row;
                row.title = setups_[index].name.empty() ? L"Unnamed config" : setups_[index].name;
                row.detail = HotkeyText(setups_[index].hotkeyModifiers, setups_[index].hotkeyVirtualKey);
                row.selected = static_cast<int>(index) == selected_;
                row.iconButtons = {kRemoveGlyph};
                rows.push_back(std::move(row));
            }
            configs_.SetRows(std::move(rows));

            std::vector<MonitorLayout::Tile> tiles;
            if (HasSelection())
            {
                for (const auto& display : setups_[static_cast<size_t>(selected_)].displayPaths)
                {
                    tiles.push_back({display.monitorName.empty() ? display.displayName : display.monitorName,
                        display.positionX, display.positionY, display.width == 0 ? 1920u : display.width,
                        display.enabled, display.enabled && display.isPrimary});
                }
            }
            layout_.SetTiles(std::move(tiles), HasSelection()
                ? L"No monitors captured yet. Click Detect current."
                : L"Select or create a config.");
        }

        void Changed()
        {
            Store();
            RefreshViews();
            Commit();
        }

        bool OnCommand(int id, int code, HWND) override
        {
            if (id == kNameId && code == EN_CHANGE && !loading_) { Changed(); return true; }
            if (id == kHotkeyId && !loading_) { Changed(); return true; }
            if (id == kNewId)
            {
                Store();
                MonitorPowerSetup setup;
                setup.name = L"New config";
                setup.displayPaths = detected_;
                if (setup.displayPaths.empty() && HasSelection()) setup.displayPaths = setups_[static_cast<size_t>(selected_)].displayPaths;
                if (setup.displayPaths.empty())
                {
                    MonitorPowerSetup current;
                    std::wstring error;
                    if (MonitorPowerController::CaptureSetup(current, &error)) detected_ = setup.displayPaths = current.displayPaths;
                }
                setups_.push_back(std::move(setup));
                selected_ = static_cast<int>(setups_.size()) - 1;
                Load();
                Commit();
                SetFocus(name_);
                SendMessageW(name_, EM_SETSEL, 0, -1);
                return true;
            }
            if (id == kConfigListId)
            {
                const int row = configs_.NotifiedRow();
                if (row < 0 || row >= static_cast<int>(setups_.size())) return true;
                if (code == RowList::kActivated && row != selected_)
                {
                    Store();
                    Commit();
                    selected_ = row;
                    Load();
                }
                else if ((code == RowList::kIconButton || code == RowList::kDeleteRequested))
                {
                    Store();
                    setups_.erase(setups_.begin() + row);
                    selected_ = setups_.empty() ? -1 : std::min(selected_, static_cast<int>(setups_.size()) - 1);
                    Load();
                    Commit();
                }
                return true;
            }
            if (id == kLayoutId && HasSelection())
            {
                auto& displays = setups_[static_cast<size_t>(selected_)].displayPaths;
                const int tile = layout_.NotifiedTile();
                if (tile < 0 || tile >= static_cast<int>(displays.size())) return true;
                auto& display = displays[static_cast<size_t>(tile)];
                if (code == MonitorLayout::kPrimary)
                {
                    for (auto& other : displays) other.isPrimary = false;
                    display.isPrimary = true;
                    display.enabled = true;
                }
                else if (code == MonitorLayout::kToggled)
                {
                    display.enabled = !display.enabled;
                    if (!display.enabled) display.isPrimary = false;
                }
                Changed();
                return true;
            }
            if (id == kDetectId)
            {
                Store();
                MonitorPowerSetup current;
                std::wstring error;
                if (!MonitorPowerController::CaptureSetup(current, &error))
                {
                    SetWindowTextW(status_, error.c_str());
                    return true;
                }
                // The selected config becomes exactly what Windows shows now.
                detected_ = current.displayPaths;
                if (HasSelection()) setups_[static_cast<size_t>(selected_)].displayPaths = current.displayPaths;
                Load();
                Commit();
                return true;
            }
            if (id == kApplyId && HasSelection())
            {
                Store();
                Commit();
                const auto problem = Validate(setups_);
                if (!problem.empty()) SetWindowTextW(status_, (L"Cannot apply yet: " + problem).c_str());
                else if (applySetup_(static_cast<size_t>(selected_))) SetWindowTextW(status_, L"Applied.");
                return true;
            }
            return false;
        }

        PageContext context_;
        std::function<bool(size_t)> applySetup_;
        std::vector<MonitorPowerSetup> setups_;
        std::vector<MonitorPowerSetup::DisplayPath> detected_;
        int selected_{-1};
        bool loading_{};
        RowList configs_;
        MonitorLayout layout_;
        HWND nameLabel_{};
        HWND name_{};
        HWND hotkeyLabel_{};
        HWND hotkey_{};
        HWND hint_{};
        HWND status_{};
    };
}

HWND CreateDisplaysPage(const PageContext& context, HWND parent, std::function<bool(size_t)> applySetup)
{
    return PageWindow::Show(std::make_unique<DisplaysPage>(context, std::move(applySetup)), context.instance, parent, 620, 420);
}
