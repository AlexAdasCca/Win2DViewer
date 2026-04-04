#pragma once

#include <windows.h>

#include <algorithm>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SystemMenu
{
    constexpr UINT_PTR kCommandWindowTopMost = 0x1F10;
    constexpr UINT_PTR kCommandAbout = 0x1F20;

    struct ShortcutBinding
    {
        UINT virtualKey = 0;
        bool ctrl = false;
        bool alt = false;
        bool shift = false;
        bool win = false;

        bool IsValid() const noexcept
        {
            switch (virtualKey)
            {
            case 0:
            case VK_CONTROL:
            case VK_MENU:
            case VK_SHIFT:
            case VK_LCONTROL:
            case VK_RCONTROL:
            case VK_LMENU:
            case VK_RMENU:
            case VK_LSHIFT:
            case VK_RSHIFT:
            case VK_LWIN:
            case VK_RWIN:
                return false;
            default:
                return true;
            }
        }

        std::wstring ToDisplayString() const
        {
            std::wstring text;
            if (ctrl)
            {
                text += L"Ctrl+";
            }
            if (alt)
            {
                text += L"Alt+";
            }
            if (shift)
            {
                text += L"Shift+";
            }
            if (win)
            {
                text += L"Win+";
            }

            if (virtualKey >= L'A' && virtualKey <= L'Z')
            {
                text.push_back(static_cast<wchar_t>(virtualKey));
                return text;
            }
            if (virtualKey >= L'0' && virtualKey <= L'9')
            {
                text.push_back(static_cast<wchar_t>(virtualKey));
                return text;
            }
            if (virtualKey >= VK_F1 && virtualKey <= VK_F24)
            {
                text += L"F" + std::to_wstring(virtualKey - VK_F1 + 1);
                return text;
            }

            switch (virtualKey)
            {
            case VK_INSERT: text += L"Insert"; break;
            case VK_DELETE: text += L"Delete"; break;
            case VK_HOME: text += L"Home"; break;
            case VK_END: text += L"End"; break;
            case VK_PRIOR: text += L"PageUp"; break;
            case VK_NEXT: text += L"PageDown"; break;
            case VK_SPACE: text += L"Space"; break;
            case VK_RETURN: text += L"Enter"; break;
            case VK_TAB: text += L"Tab"; break;
            default:
                text += L"VK_" + std::to_wstring(virtualKey);
                break;
            }

            return text;
        }

        std::wstring ToNormalizedKey() const
        {
            std::wstring text;
            if (ctrl) text += L"ctrl+";
            if (alt) text += L"alt+";
            if (shift) text += L"shift+";
            if (win) text += L"win+";
            text += std::to_wstring(virtualKey);
            return text;
        }

        bool Matches(UINT message, WPARAM wParam) const noexcept
        {
            if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN)
            {
                return false;
            }

            if (static_cast<UINT>(wParam) != virtualKey)
            {
                return false;
            }

            const auto IsPressed = [](int vk) -> bool
            {
                return (::GetKeyState(vk) & 0x8000) != 0;
            };

            const bool ctrlPressed = IsPressed(VK_CONTROL);
            const bool altPressed = IsPressed(VK_MENU);
            const bool shiftPressed = IsPressed(VK_SHIFT);
            const bool winPressed = IsPressed(VK_LWIN) || IsPressed(VK_RWIN);

            return ctrlPressed == ctrl &&
                altPressed == alt &&
                shiftPressed == shift &&
                winPressed == win;
        }
    };

    struct MenuItemSpec
    {
        UINT_PTR id = 0;
        std::wstring text;
        std::optional<ShortcutBinding> shortcut;
        HBITMAP bitmap = nullptr;
        bool separator = false;
        std::function<void(HWND)> onInvoke;
        std::function<bool()> isChecked;
        std::function<bool()> isEnabled;
    };

    namespace Internal
    {
        struct RegisteredShortcut
        {
            std::wstring owner;
            UINT_PTR commandId = 0;
            std::wstring label;
        };

        inline std::mutex gShortcutRegistryMutex;
        inline std::unordered_map<std::wstring, RegisteredShortcut> gShortcutRegistry;

        inline std::wstring ToLower(std::wstring text)
        {
            std::transform(text.begin(), text.end(), text.begin(), towlower);
            return text;
        }
    }

    class MenuHost
    {
    public:
        explicit MenuHost(std::wstring scopeName)
            : scopeName_(std::move(scopeName))
        {
        }

        ~MenuHost()
        {
            UnregisterShortcuts();
        }

        MenuHost(MenuHost const&) = delete;
        MenuHost& operator=(MenuHost const&) = delete;

        bool AddItem(MenuItemSpec item, std::wstring* errorMessage = nullptr)
        {
            return InsertItem(items_.size(), std::move(item), errorMessage);
        }

        bool InsertItem(size_t index, MenuItemSpec item, std::wstring* errorMessage = nullptr)
        {
            if (!ValidateItem(item, errorMessage, nullptr))
            {
                return false;
            }

            if (index > items_.size())
            {
                index = items_.size();
            }

            items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(index), std::move(item));
            return true;
        }

        bool UpdateItem(UINT_PTR id, MenuItemSpec item, std::wstring* errorMessage = nullptr)
        {
            MenuItemSpec* existing = FindItem(id);
            if (existing == nullptr)
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = L"Menu item was not found.";
                }
                return false;
            }

            if (!ValidateItem(item, errorMessage, &id))
            {
                return false;
            }

            *existing = std::move(item);
            return true;
        }

        bool RemoveItem(UINT_PTR id)
        {
            auto it = std::find_if(items_.begin(), items_.end(), [&](MenuItemSpec const& item) { return item.id == id; });
            if (it == items_.end())
            {
                return false;
            }

            items_.erase(it);
            return true;
        }

        MenuItemSpec* FindItem(UINT_PTR id)
        {
            auto it = std::find_if(items_.begin(), items_.end(), [&](MenuItemSpec& item) { return item.id == id; });
            return it != items_.end() ? &(*it) : nullptr;
        }

        MenuItemSpec const* FindItem(UINT_PTR id) const
        {
            auto it = std::find_if(items_.begin(), items_.end(), [&](MenuItemSpec const& item) { return item.id == id; });
            return it != items_.end() ? &(*it) : nullptr;
        }

        std::vector<MenuItemSpec> const& Items() const noexcept
        {
            return items_;
        }

        void Clear()
        {
            UnregisterShortcuts();
            items_.clear();
        }

        bool Install(HMENU menuHandle, std::wstring* errorMessage = nullptr)
        {
            if (menuHandle == nullptr)
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = L"Menu handle is null.";
                }
                return false;
            }

            itemTexts_.clear();
            UnregisterShortcuts();
            if (!RegisterShortcuts(errorMessage))
            {
                return false;
            }

            for (MenuItemSpec const& item : items_)
            {
                MENUITEMINFOW menuItemInfo{};
                menuItemInfo.cbSize = sizeof(menuItemInfo);

                if (item.separator)
                {
                    menuItemInfo.fMask = MIIM_FTYPE;
                    menuItemInfo.fType = MFT_SEPARATOR;
                }
                else
                {
                    itemTexts_.push_back(BuildMenuText(item));
                    menuItemInfo.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STRING | MIIM_STATE;
                    menuItemInfo.wID = static_cast<UINT>(item.id);
                    menuItemInfo.fType = MFT_STRING;
                    menuItemInfo.dwTypeData = itemTexts_.back().data();
                    menuItemInfo.fState = MFS_ENABLED;

                    if (item.bitmap != nullptr)
                    {
                        menuItemInfo.fMask |= MIIM_BITMAP;
                        menuItemInfo.hbmpItem = item.bitmap;
                    }
                }

                if (!::InsertMenuItemW(menuHandle, static_cast<UINT>(::GetMenuItemCount(menuHandle)), TRUE, &menuItemInfo))
                {
                    if (errorMessage != nullptr)
                    {
                        *errorMessage = L"Failed to insert system menu item.";
                    }
                    return false;
                }
            }

            RefreshState(menuHandle);
            return true;
        }

        void RefreshState(HMENU menuHandle) const
        {
            if (menuHandle == nullptr)
            {
                return;
            }

            for (MenuItemSpec const& item : items_)
            {
                if (item.separator)
                {
                    continue;
                }

                const bool checked = item.isChecked ? item.isChecked() : false;
                const bool enabled = item.isEnabled ? item.isEnabled() : true;

                ::CheckMenuItem(
                    menuHandle,
                    static_cast<UINT>(item.id),
                    MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));

                ::EnableMenuItem(
                    menuHandle,
                    static_cast<UINT>(item.id),
                    MF_BYCOMMAND | (enabled ? MF_ENABLED : (MF_DISABLED | MF_GRAYED)));
            }
        }

        bool HandleCommand(HWND windowHandle, UINT_PTR commandId) const
        {
            MenuItemSpec const* item = FindItem(commandId);
            if (item == nullptr || item->separator || !item->onInvoke)
            {
                return false;
            }

            if (item->isEnabled && !item->isEnabled())
            {
                return false;
            }

            item->onInvoke(windowHandle);
            return true;
        }

        bool HandleShortcut(HWND windowHandle, UINT message, WPARAM wParam) const
        {
            for (MenuItemSpec const& item : items_)
            {
                if (!item.shortcut.has_value() || item.separator)
                {
                    continue;
                }

                if (item.shortcut->Matches(message, wParam))
                {
                    if (item.isEnabled && !item.isEnabled())
                    {
                        return false;
                    }

                    if (item.onInvoke)
                    {
                        item.onInvoke(windowHandle);
                        return true;
                    }
                }
            }

            return false;
        }

    private:
        std::wstring BuildMenuText(MenuItemSpec const& item) const
        {
            std::wstring text = item.text;
            if (item.shortcut.has_value())
            {
                text += L"\t";
                text += item.shortcut->ToDisplayString();
            }
            return text;
        }

        bool ValidateItem(MenuItemSpec const& item, std::wstring* errorMessage, UINT_PTR const* ignoredId) const
        {
            if (item.separator)
            {
                return true;
            }

            if (item.id == 0)
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = L"Menu command id must not be zero.";
                }
                return false;
            }

            if (item.text.empty())
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = L"Menu item text must not be empty.";
                }
                return false;
            }

            for (MenuItemSpec const& existing : items_)
            {
                if (&existing == &item)
                {
                    continue;
                }

                if (ignoredId != nullptr && existing.id == *ignoredId)
                {
                    continue;
                }

                if (!existing.separator && existing.id == item.id)
                {
                    if (errorMessage != nullptr)
                    {
                        *errorMessage = L"Menu command id conflict detected.";
                    }
                    return false;
                }

                if (item.shortcut.has_value() && existing.shortcut.has_value() &&
                    item.shortcut->ToNormalizedKey() == existing.shortcut->ToNormalizedKey())
                {
                    if (errorMessage != nullptr)
                    {
                        *errorMessage = L"Shortcut conflict detected in the current menu model.";
                    }
                    return false;
                }
            }

            if (item.shortcut.has_value() && !item.shortcut->IsValid())
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = L"Shortcut binding is invalid.";
                }
                return false;
            }

            return true;
        }

        bool RegisterShortcuts(std::wstring* errorMessage)
        {
            std::scoped_lock lock(Internal::gShortcutRegistryMutex);
            registeredShortcutKeys_.clear();

            for (MenuItemSpec const& item : items_)
            {
                if (!item.shortcut.has_value())
                {
                    continue;
                }

                const std::wstring key = item.shortcut->ToNormalizedKey();
                auto it = Internal::gShortcutRegistry.find(key);
                if (it != Internal::gShortcutRegistry.end() && it->second.owner != scopeName_)
                {
                    if (errorMessage != nullptr)
                    {
                        *errorMessage = L"Shortcut conflicts with another menu in this process: " + item.shortcut->ToDisplayString();
                    }
                    for (std::wstring const& registeredKey : registeredShortcutKeys_)
                    {
                        Internal::gShortcutRegistry.erase(registeredKey);
                    }
                    registeredShortcutKeys_.clear();
                    return false;
                }

                Internal::gShortcutRegistry[key] = Internal::RegisteredShortcut{ scopeName_, item.id, item.text };
                registeredShortcutKeys_.push_back(key);
            }

            return true;
        }

        void UnregisterShortcuts()
        {
            std::scoped_lock lock(Internal::gShortcutRegistryMutex);
            for (std::wstring const& key : registeredShortcutKeys_)
            {
                auto it = Internal::gShortcutRegistry.find(key);
                if (it != Internal::gShortcutRegistry.end() && it->second.owner == scopeName_)
                {
                    Internal::gShortcutRegistry.erase(it);
                }
            }
            registeredShortcutKeys_.clear();
        }

    private:
        std::wstring scopeName_;
        std::vector<MenuItemSpec> items_;
        mutable std::vector<std::wstring> itemTexts_;
        std::vector<std::wstring> registeredShortcutKeys_;
    };
}
