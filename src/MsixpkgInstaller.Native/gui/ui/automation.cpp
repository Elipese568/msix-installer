#include "automation.h"

#include "controls.h"
#include "host.h"

#include <oleacc.h>
#include <oleauto.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <string>

namespace msixpkg::ui {
namespace {

long msaa_role(Role role) {
    switch (role) {
        case Role::Label: return ROLE_SYSTEM_STATICTEXT;
        case Role::Button: return ROLE_SYSTEM_PUSHBUTTON;
        case Role::Checkbox: return ROLE_SYSTEM_CHECKBUTTON;
        case Role::Progress: return ROLE_SYSTEM_PROGRESSBAR;
        case Role::Text: return ROLE_SYSTEM_TEXT;
        case Role::Table: return ROLE_SYSTEM_LIST;
        case Role::Row: return ROLE_SYSTEM_LISTITEM;
        case Role::ScrollBar: return ROLE_SYSTEM_SCROLLBAR;
        case Role::Panel: return ROLE_SYSTEM_PANE;
    }

    return ROLE_SYSTEM_CLIENT;
}

DWORD msaa_state(uint32_t state) {
    DWORD result = 0;
    if ((state & kStateFocusable) != 0) {
        result |= STATE_SYSTEM_FOCUSABLE;
    }

    if ((state & kStateFocused) != 0) {
        result |= STATE_SYSTEM_FOCUSED;
    }

    if ((state & kStateDisabled) != 0) {
        result |= STATE_SYSTEM_UNAVAILABLE;
    }

    if ((state & kStateChecked) != 0) {
        result |= STATE_SYSTEM_CHECKED;
    }

    if ((state & kStateSelected) != 0) {
        result |= STATE_SYSTEM_SELECTED;
    }

    if ((state & kStateReadOnly) != 0) {
        result |= STATE_SYSTEM_READONLY;
    }

    if ((state & kStateInvisible) != 0) {
        result |= STATE_SYSTEM_INVISIBLE;
    }

    if ((state & kStateOffscreen) != 0) {
        result |= STATE_SYSTEM_OFFSCREEN;
    }

    return result;
}

HRESULT return_bstr(const std::wstring& value, BSTR* out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }

    *out = value.empty() ? nullptr : ::SysAllocString(value.c_str());
    return value.empty() ? S_FALSE : S_OK;
}

}  // namespace

void AccessibleReleaser::operator()(IAccessible* value) const noexcept {
    if (value != nullptr) {
        value->Release();
    }
}

class AccessibleObject final : public IAccessible {
public:
    AccessibleObject(Host* host, int child_id) : host_(host), child_(child_id) {}

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_INVALIDARG;
        }

        *object = nullptr;
        if (::IsEqualIID(riid, IID_IUnknown) || ::IsEqualIID(riid, IID_IDispatch) ||
            ::IsEqualIID(riid, IID_IAccessible)) {
            *object = static_cast<IAccessible*>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            delete this;
        }

        return remaining;
    }

    // ---- IDispatch ----
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
        if (count == nullptr) {
            return E_INVALIDARG;
        }

        *count = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo** info) override {
        if (info != nullptr) {
            *info = nullptr;
        }

        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT count, LCID, DISPID* ids) override {
        if (ids == nullptr) {
            return E_INVALIDARG;
        }

        for (UINT index = 0; names != nullptr && index < count; ++index) {
            ids[index] = DISPID_UNKNOWN;
        }

        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*,
                                    UINT*) override {
        return E_NOTIMPL;
    }

    // ---- IAccessible ----
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override {
        if (parent == nullptr) {
            return E_INVALIDARG;
        }

        *parent = nullptr;
        if (child_ > 0) {
            auto* root = new AccessibleObject(host_, 0);
            *parent = static_cast<IDispatch*>(root);
            return S_OK;
        }

        if (host_ == nullptr || host_->hwnd() == nullptr) {
            return S_FALSE;
        }

        return ::AccessibleObjectFromWindow(host_->hwnd(), OBJID_WINDOW, IID_IAccessible,
                                            reinterpret_cast<void**>(parent));
    }

    HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override {
        if (count == nullptr) {
            return E_INVALIDARG;
        }

        *count = 0;
        if (child_ != 0 || host_ == nullptr) {
            return S_OK;
        }

        *count = static_cast<long>(host_->accessible_entries().size());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child, IDispatch** dispatch) override {
        if (dispatch == nullptr) {
            return E_INVALIDARG;
        }

        *dispatch = nullptr;
        if (child_ != 0 || host_ == nullptr) {
            return S_FALSE;
        }

        if (child.vt != VT_I4 || child.lVal < 1) {
            return E_INVALIDARG;
        }

        const auto entries = host_->accessible_entries();
        if (static_cast<size_t>(child.lVal) > entries.size()) {
            return E_INVALIDARG;
        }

        auto* object = new AccessibleObject(host_, static_cast<int>(child.lVal));
        *dispatch = static_cast<IDispatch*>(object);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override {
        const HRESULT valid = validate(child);
        if (FAILED(valid)) {
            return valid;
        }

        if (child_ == 0) {
            if (host_ == nullptr || host_->hwnd() == nullptr) {
                return S_FALSE;
            }

            wchar_t title[256]{};
            ::GetWindowTextW(host_->hwnd(), title, static_cast<int>(std::size(title)));
            return return_bstr(title, name);
        }

        const Host::AccessibleEntry current = entry();
        if (current.widget == nullptr) {
            return S_FALSE;
        }

        return return_bstr(entry_name(current), name);
    }

    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* value) override {
        const HRESULT valid = validate(child);
        if (FAILED(valid)) {
            return valid;
        }

        if (child_ == 0) {
            return S_FALSE;
        }

        const Host::AccessibleEntry current = entry();
        if (current.widget == nullptr) {
            return S_FALSE;
        }

        if (current.row >= 0) {
            if (auto* table = dynamic_cast<Table*>(current.widget)) {
                return return_bstr(table->row_text(current.row), value);
            }

            return S_FALSE;
        }

        return return_bstr(current.widget->accessible_value(), value);
    }

    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR* description) override {
        if (description != nullptr) {
            *description = nullptr;
        }

        return S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override {
        const HRESULT valid = validate(child);
        if (FAILED(valid)) {
            return valid;
        }

        if (role == nullptr) {
            return E_INVALIDARG;
        }

        ::VariantInit(role);
        role->vt = VT_I4;
        if (child_ == 0) {
            role->lVal = ROLE_SYSTEM_PANE;
            return S_OK;
        }

        const Host::AccessibleEntry current = entry();
        if (current.widget == nullptr) {
            return S_FALSE;
        }

        role->lVal = current.row >= 0 ? ROLE_SYSTEM_LISTITEM : msaa_role(current.widget->accessible_role());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override {
        const HRESULT valid = validate(child);
        if (FAILED(valid)) {
            return valid;
        }

        if (state == nullptr) {
            return E_INVALIDARG;
        }

        ::VariantInit(state);
        state->vt = VT_I4;
        if (child_ == 0) {
            state->lVal = 0;
            return S_OK;
        }

        const Host::AccessibleEntry current = entry();
        if (current.widget == nullptr) {
            return S_FALSE;
        }

        DWORD value = 0;
        if (current.row >= 0) {
            value = STATE_SYSTEM_SELECTABLE | STATE_SYSTEM_FOCUSABLE;
            auto* table = dynamic_cast<Table*>(current.widget);
            if (table == nullptr) {
                state->lVal = static_cast<LONG>(value);
                return S_OK;
            }

            if (table->selected_row() == current.row) {
                value |= STATE_SYSTEM_SELECTED;
            }

            const RectF row = table->row_bounds(current.row);
            const RectF view = table->row_view_bounds();
            if (row.bottom() <= view.y || row.y >= view.bottom()) {
                value |= STATE_SYSTEM_OFFSCREEN;
            }
        } else {
            value = msaa_state(current.widget->accessible_state());
            if (host_ != nullptr && host_->focused_widget() == current.widget) {
                value |= STATE_SYSTEM_FOCUSED;
            }
        }

        state->lVal = static_cast<LONG>(value);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR* help) override {
        if (help != nullptr) {
            *help = nullptr;
        }

        return S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* help_file, VARIANT, long* topic) override {
        if (help_file != nullptr) {
            *help_file = nullptr;
        }

        if (topic != nullptr) {
            *topic = 0;
        }

        return S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR* shortcut) override {
        if (shortcut != nullptr) {
            *shortcut = nullptr;
        }

        return S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* focus) override {
        if (focus == nullptr) {
            return E_INVALIDARG;
        }

        ::VariantInit(focus);
        if (host_ == nullptr || host_->focused_widget() == nullptr) {
            return S_FALSE;
        }

        if (child_ == 0) {
            const int id = host_->child_id_of(host_->focused_widget());
            if (id <= 0) {
                return S_FALSE;
            }

            focus->vt = VT_I4;
            focus->lVal = id;
            return S_OK;
        }

        const Host::AccessibleEntry current = entry();
        if (current.row < 0 && current.widget == host_->focused_widget()) {
            focus->vt = VT_I4;
            focus->lVal = CHILDID_SELF;
            return S_OK;
        }

        return S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* selection) override {
        if (selection == nullptr) {
            return E_INVALIDARG;
        }

        ::VariantInit(selection);
        if (host_ == nullptr) {
            return S_FALSE;
        }

        if (child_ == 0) {
            const auto entries = host_->accessible_entries();
            for (size_t index = 0; index < entries.size(); ++index) {
                if (entries[index].row < 0) {
                    continue;
                }

                auto* table = dynamic_cast<Table*>(entries[index].widget);
                if (table != nullptr && table->selected_row() == entries[index].row) {
                    selection->vt = VT_I4;
                    selection->lVal = static_cast<LONG>(index) + 1;
                    return S_OK;
                }
            }

            return S_FALSE;
        }

        const Host::AccessibleEntry current = entry();
        if (current.row >= 0) {
            if (auto* table = dynamic_cast<Table*>(current.widget);
                table != nullptr && table->selected_row() == current.row) {
                selection->vt = VT_I4;
                selection->lVal = CHILDID_SELF;
                return S_OK;
            }
        }

        return S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* action) override {
        const HRESULT valid = validate(child);
        if (FAILED(valid)) {
            return valid;
        }

        if (child_ == 0) {
            if (action != nullptr) {
                *action = nullptr;
            }

            return S_FALSE;
        }

        const Host::AccessibleEntry current = entry();
        if (current.widget == nullptr) {
            return S_FALSE;
        }

        if (current.row >= 0) {
            return return_bstr(L"选择", action);
        }

        return return_bstr(current.widget->accessible_default_action(), action);
    }

    HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT child) override {
        const HRESULT valid = validate(child);
        if (FAILED(valid)) {
            return valid;
        }

        const Host::AccessibleEntry current = entry();
        if (current.widget == nullptr || host_ == nullptr) {
            return S_FALSE;
        }

        if ((flags & SELFLAG_TAKEFOCUS) != 0) {
            host_->focus_widget(current.widget);
        }

        if ((flags & SELFLAG_TAKESELECTION) != 0 && current.row >= 0) {
            if (auto* table = dynamic_cast<Table*>(current.widget)) {
                table->set_selected_row(current.row);
            }
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE accLocation(long* left, long* top, long* width, long* height,
                                           VARIANT child) override {
        const HRESULT valid = validate(child);
        if (FAILED(valid)) {
            return valid;
        }

        if (left == nullptr || top == nullptr || width == nullptr || height == nullptr) {
            return E_INVALIDARG;
        }

        if (host_ == nullptr) {
            return E_FAIL;
        }

        RectF bounds;
        if (child_ == 0) {
            bounds = {0.0f, 0.0f, static_cast<float>(host_->client_width_px()),
                      static_cast<float>(host_->client_height_px())};
            bounds.width = host_->dip(bounds.width);
            bounds.height = host_->dip(bounds.height);
        } else {
            const Host::AccessibleEntry current = entry();
            if (current.widget == nullptr) {
                return S_FALSE;
            }

            if (current.row >= 0) {
                auto* table = dynamic_cast<Table*>(current.widget);
                if (table == nullptr) {
                    return S_FALSE;
                }

                bounds = table->row_bounds(current.row);
            } else {
                bounds = current.widget->bounds;
            }
        }

        const RectF screen = host_->to_screen(bounds);
        *left = static_cast<long>(std::lround(screen.x));
        *top = static_cast<long>(std::lround(screen.y));
        *width = static_cast<long>(std::lround(screen.width));
        *height = static_cast<long>(std::lround(screen.height));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT start, VARIANT* destination) override {
        if (destination == nullptr) {
            return E_INVALIDARG;
        }

        ::VariantInit(destination);
        if (host_ == nullptr) {
            return S_FALSE;
        }

        if (child_ != 0) {
            if (direction == NAVDIR_UP) {
                auto* root = new AccessibleObject(host_, 0);
                destination->vt = VT_DISPATCH;
                destination->pdispVal = static_cast<IDispatch*>(root);
                return S_OK;
            }

            return S_FALSE;
        }

        const auto entries = host_->accessible_entries();
        const long count = static_cast<long>(entries.size());
        const long from = start.vt == VT_I4 ? start.lVal : 0;
        long target = 0;
        switch (direction) {
            case NAVDIR_FIRSTCHILD:
                target = count > 0 ? 1 : 0;
                break;
            case NAVDIR_LASTCHILD:
                target = count;
                break;
            case NAVDIR_NEXT:
                target = (from >= 1 && from < count) ? from + 1 : 0;
                break;
            case NAVDIR_PREVIOUS:
                target = from > 1 ? from - 1 : 0;
                break;
            default:
                return S_FALSE;
        }

        if (target <= 0) {
            return S_FALSE;
        }

        destination->vt = VT_I4;
        destination->lVal = target;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT* child) override {
        if (child == nullptr) {
            return E_INVALIDARG;
        }

        ::VariantInit(child);
        if (host_ == nullptr || host_->hwnd() == nullptr || host_->root() == nullptr) {
            return S_FALSE;
        }

        POINT point{x, y};
        ::ScreenToClient(host_->hwnd(), &point);

        Widget* hit = host_->root()->hit_test(host_->dip(static_cast<float>(point.x)),
                                              host_->dip(static_cast<float>(point.y)));
        if (hit == nullptr || hit->accessible_role() == Role::Panel) {
            child->vt = VT_I4;
            child->lVal = CHILDID_SELF;
            return S_OK;
        }

        const auto entries = host_->accessible_entries();
        for (size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].widget != hit) {
                continue;
            }

            if (entries[index].row < 0) {
                child->vt = VT_I4;
                child->lVal = static_cast<LONG>(index) + 1;
                return S_OK;
            }
        }

        child->vt = VT_I4;
        child->lVal = CHILDID_SELF;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
        const HRESULT valid = validate(child);
        if (FAILED(valid)) {
            return valid;
        }

        if (child_ == 0) {
            return S_FALSE;
        }

        const Host::AccessibleEntry current = entry();
        if (current.widget == nullptr) {
            return S_FALSE;
        }

        if (current.row >= 0) {
            if (auto* table = dynamic_cast<Table*>(current.widget)) {
                table->set_selected_row(current.row);
            }

            return S_OK;
        }

        current.widget->do_default_action();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }

private:
    Host::AccessibleEntry entry() const {
        Host::AccessibleEntry result;
        if (host_ == nullptr || child_ <= 0) {
            return result;
        }

        const auto entries = host_->accessible_entries();
        const size_t index = static_cast<size_t>(child_ - 1);
        if (index < entries.size()) {
            result = entries[index];
        }

        return result;
    }

    std::wstring entry_name(const Host::AccessibleEntry& current) const {
        if (current.widget == nullptr) {
            return {};
        }

        if (current.row >= 0) {
            if (auto* table = dynamic_cast<Table*>(current.widget)) {
                return table->row_text(current.row);
            }

            return {};
        }

        return current.widget->accessible_name();
    }

    HRESULT validate(const VARIANT& child) const {
        if (child.vt != VT_I4) {
            return E_INVALIDARG;
        }

        if (child_ == 0) {
            return child.lVal == CHILDID_SELF ? S_OK : E_INVALIDARG;
        }

        return (child.lVal == CHILDID_SELF || child.lVal == child_) ? S_OK : E_INVALIDARG;
    }

    std::atomic<ULONG> references_{1};
    Host* host_ = nullptr;
    int child_ = 0;
};

AccessiblePtr create_accessibility_root(Host* host) {
    return AccessiblePtr(new AccessibleObject(host, 0));
}

}  // namespace msixpkg::ui
