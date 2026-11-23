#pragma once

#include "../columns_ui-sdk-main/ui_extension.h"
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pss {

// {4532EC89-C567-4267-B33A-0C154B6F0105}
constexpr GUID kPssExtensionGuid = {
    0x4532ec89, 0xc567, 0x4267, {0xb3, 0x3a, 0x0c, 0x15, 0x4b, 0x6f, 0x01, 0x05}
};

struct pss_child_entry {
    GUID panel_guid = pfc::guid_null;
    std::string name;

    // 68-byte metadata prefix (matches upstream PSS config stream byte-for-byte)
    int32_t ver = 1;
    int32_t forced_layout = 0;
    int32_t keep_aspect = 0;
    RECT saved_rect{};
    int32_t mid_ints[8]{ 0, 0, 0, 0, 5, 5, 5, 5 };
    double weight = 100.0;

    // 4 layout expressions (empty when non-forced)
    std::string expr_x;
    std::string expr_y;
    std::string expr_w;
    std::string expr_h;

    // 16-byte metadata suffix (12 preserved bytes + u32 hidden_on_start)
    uint8_t meta_pad[12]{};
    uint32_t hidden_on_start = 0;

    // Child panel configuration stream
    pfc::array_t<t_uint8> cfg_data;

    // Live runtime state
    uie::window_ptr window;
    HWND child_hwnd = nullptr;
};

class panel_stack_splitter;

class pss_window_host : public uie::window_host_ex {
public:
    pss_window_host() = default;
    void set_owner(panel_stack_splitter* owner) { m_owner = owner; }

    const GUID& get_host_guid() const override;
    void on_size_limit_change(HWND wnd, unsigned flags) override;
    unsigned is_resize_supported(HWND wnd) const override;
    bool request_resize(HWND wnd, unsigned flags, unsigned width, unsigned height) override;
    bool override_status_text_create(service_ptr_t<ui_status_text_override>& p_out) override;
    bool get_keyboard_shortcuts_enabled() const override;
    bool is_visible(HWND wnd) const override;
    bool is_visibility_modifiable(HWND wnd, bool desired_visibility) const override;
    bool set_window_visibility(HWND wnd, bool visibility) override;
    void relinquish_ownership(HWND wnd) override;
    void get_children(pfc::list_base_t<uie::window_ptr>& p_out) override;

private:
    panel_stack_splitter* m_owner = nullptr;
};

class panel_stack_splitter : public uie::splitter_window_v2 {
public:
    panel_stack_splitter();
    ~panel_stack_splitter();

    // uie::extension_base
    const GUID& get_extension_guid() const override { return kPssExtensionGuid; }
    void get_name(pfc::string_base& out) const override { out = "Panel Stack Splitter"; }
    void set_config(stream_reader* p_reader, t_size p_size, abort_callback& p_abort) override;
    void get_config(stream_writer* p_writer, abort_callback& p_abort) const override;

    // uie::window
    void get_category(pfc::string_base& out) const override { out = "Splitters"; }
    unsigned get_type() const override { return uie::type_panel | uie::type_splitter; }
    bool is_available(const uie::window_host_ptr& p_host) const override { return true; }
    HWND create_or_transfer_window(HWND wnd_parent, const uie::window_host_ptr& p_host,
        const ui_helpers::window_position_t& p_position) override;
    void destroy_window() override;
    HWND get_wnd() const override { return m_wnd; }

    // uie::splitter_window
    bool get_config_item_supported(t_size p_index, const GUID& p_type) const override;
    bool get_config_item(t_size index, const GUID& p_type, stream_writer* p_out, abort_callback& p_abort) const override;
    bool set_config_item(t_size index, const GUID& p_type, stream_reader* p_source, abort_callback& p_abort) override;
    void insert_panel(t_size index, const uie::splitter_item_t* p_item) override;
    void remove_panel(t_size index) override;
    void replace_panel(t_size index, const uie::splitter_item_t* p_item) override;
    t_size get_panel_count() const override { return m_children.size(); }
    uie::splitter_item_t* get_panel(t_size index) const override;

    // uie::splitter_window_v2
    bool is_point_ours(HWND wnd_point, const POINT& pt_screen, pfc::list_base_t<uie::window_ptr>& p_hierarchy) override;
    void get_supported_panels(
        const pfc::list_base_const_t<uie::window_ptr>& p_windows, bit_array_var& p_mask_unsupported) override;

    // Host callbacks
    const uie::window_host_ptr& parent_host() const { return m_host; }
    void on_child_relinquished(HWND wnd);
    void on_child_visibility_changed(HWND wnd);
    void collect_children(pfc::list_base_t<uie::window_ptr>& p_out) const;
    void relayout();

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp);

    void reset_defaults();
    void ensure_window_class();
    void create_children();
    void destroy_children();
    void instantiate_child(pss_child_entry& entry, int parent_w, int parent_h, HWND prev_z);
    void compute_child_rects(int w, int h, std::vector<RECT>& out_rects);
    void read_item_into_entry(const uie::splitter_item_t* src, pss_child_entry& dst);
    void refresh_live_configs() const;
    void fill_solid_bg(HDC hdc, const RECT& rc);

    HWND m_wnd = nullptr;
    uie::window_host_ptr m_host;
    service_ptr_t<pss_window_host> m_child_host;

    int32_t m_hdr_ints[3]{ 0, 1, 0 };
    COLORREF m_bg_color = RGB(40, 40, 40);
    HBRUSH m_bg_brush = nullptr;
    COLORREF m_bg_brush_color = CLR_INVALID;
    int32_t m_hdr_ints2[2]{ 0, 0 };
    std::string m_script;
    pfc::array_t<t_uint8> m_hdr_tail;
    mutable std::vector<pss_child_entry> m_children;
    bool m_in_layout = false;
};

} // namespace pss
