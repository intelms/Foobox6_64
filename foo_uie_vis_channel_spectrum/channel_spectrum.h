#pragma once

#include "../columns_ui-sdk-main/ui_extension.h"
#include "../foobar2000/SDK/vis.h"
#include <windows.h>

#include <array>
#include <cstdint>
#include <vector>

namespace cs {

// {50C61B3C-7CF3-4988-84D0-9A2C508FB8F0}
constexpr GUID kChannelSpectrumGuid = {
    0x50c61b3c, 0x7cf3, 0x4988, {0x84, 0xd0, 0x9a, 0x2c, 0x50, 0x8f, 0xb8, 0xf0}
};

enum ColorMode : uint32_t {
    ColorSimple = 0,
    ColorHorizontal = 1,
    ColorVertical = 2,
    ColorSimpleAA = 3,
    ColorHorizontalAA = 4,
    ColorCurve = 5,
};

enum BorderStyle : uint32_t {
    BorderNone = 0,
    BorderSunken = 1,
    BorderGrey = 2,
};

enum ChannelMode : uint32_t {
    ChannelSpread = 0,
    ChannelGrouped = 1,
    ChannelCustom = 2,
};

// Mirrors the legacy 74-dword (296-byte) stream layout byte-for-byte.
struct cs_config {
    std::array<uint32_t, 74> raw{};

    COLORREF fg1 = RGB(0xe4, 0xe4, 0xe4);
    COLORREF fg2 = RGB(0x20, 0x20, 0x20);
    COLORREF bg = RGB(0x00, 0x00, 0x00);
    COLORREF peak_color = RGB(0xff, 0xff, 0xff);
    COLORREF ruler_color = RGB(0xf5, 0xf5, 0xf5);
    std::array<COLORREF, 8> fg_stops{
        RGB(0x99, 0x11, 0x11), RGB(0xc6, 0x22, 0x15), RGB(0xce, 0x42, 0x1a), RGB(0xd7, 0x83, 0x11),
        RGB(0xad, 0x9a, 0x16), RGB(0x89, 0x8e, 0x28), RGB(0x31, 0x8c, 0x76), RGB(0x36, 0x64, 0x7a),
    };

    bool show_peak = true;
    uint32_t color_mode = ColorVertical;
    uint32_t border_style = BorderNone;
    uint32_t bar_width = 10;
    bool transparent = false;
    bool fast_pseudo_transparent = false;
    bool log_amp = true;
    bool linear_freq = false;
    bool freq_flip = false;
    uint32_t refresh_ms = 25;
    uint32_t fft_size = 2048;
    uint32_t peak_hold_ms = 100;   // raw[44], stored in ms (dialog unit = 10ms)
    bool grid_mode = true;
    uint32_t channel_mode = ChannelSpread;
    uint32_t channel_single = 0;   // raw[15]
    bool chan_flip = false;
    uint32_t rotation = 0;         // 0=0, 1=90, 2=180, 3=270
    uint32_t scale_pct = 100;
    uint32_t spacing = 0;
    uint32_t peak_fall_speed = 10; // raw[12], range 1..99
    uint32_t blur_level = 25;      // raw[21], range 1..100
    bool use_custom_fg = false;
    bool use_custom_bg = false;
    std::array<COLORREF, 8> bg_stops{
        RGB(0x00, 0x00, 0x00), RGB(0x00, 0x00, 0x00), RGB(0x00, 0x00, 0x00), RGB(0x00, 0x00, 0x00),
        RGB(0x00, 0x00, 0x00), RGB(0x00, 0x00, 0x00), RGB(0x00, 0x00, 0x00), RGB(0x00, 0x00, 0x00),
    };
    bool sync_scanline = false;    // raw[45]
    bool show_freq_ruler = false;
    bool show_amp_ruler = false;
    std::array<uint32_t, 18> channel_order{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17
    };
    uint32_t channel_dir = 0;      // raw[64]: 0=反射(对称), 1=右, 2=左
    uint32_t channel_mask = 0x3ffffu;
    uint32_t channel_spacing = 0;  // raw[68]: px gap between channels
    uint32_t pad_left = 5;
    uint32_t pad_right = 5;
    bool fft_new = true;

    cs_config();
    void reset_defaults();
    void unpack_from_raw();
    void pack_to_raw();
    void read_stream(stream_reader* r, t_size size, abort_callback& abort);
    void write_stream(stream_writer* w, abort_callback& abort) const;
};

class channel_spectrum_window : public uie::window {
public:
    channel_spectrum_window() = default;
    ~channel_spectrum_window();

    // service_base / extension_base
    const GUID& get_extension_guid() const override { return kChannelSpectrumGuid; }
    void get_name(pfc::string_base& out) const override { out = "Channel spectrum panel"; }
    void set_config(stream_reader* p_reader, t_size p_size, abort_callback& p_abort) override;
    void get_config(stream_writer* p_writer, abort_callback& p_abort) const override;
    bool have_config_popup() const override { return true; }
    bool show_config_popup(HWND wnd_parent) override;

    // uie::window
    void get_category(pfc::string_base& out) const override { out = "Visualisations"; }
    unsigned get_type() const override { return uie::type_panel; }
    bool is_available(const uie::window_host_ptr& /*p_host*/) const override { return true; }
    HWND create_or_transfer_window(HWND wnd_parent, const uie::window_host_ptr& p_host,
        const ui_helpers::window_position_t& p_position) override;
    void destroy_window() override;
    HWND get_wnd() const override { return m_wnd; }

    const cs_config& config() const { return m_cfg; }
    void apply_config(const cs_config& next);

private:
    struct bar_state {
        double level = 0.0;       // smoothed bar height 0..1
        double peak = 0.0;        // peak height 0..1
        double peak_hold_ms = 0.0;
    };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp);

    void ensure_window_class();
    void recreate_vis_stream();
    void restart_timer();
    void stop_timer();
    void destroy_gdi_cache();
    void ensure_gdi_cache(HDC ref_dc, int w, int h);
    void rebuild_gradient_bitmap(HDC ref_dc, int w, int h);
    void invalidate_parent_bg();
    void ensure_parent_bg(HDC ref_dc, int w, int h);
    int effective_transparent_mode() const; // 0=opaque, 1=SetWindowRgn, 3=fast pseudo
    void collect_mode1_bands(int w, int h, std::vector<RECT>& out_bands) const;
    void compute_channel_slot_rects(const RECT& inner, size_t slot_cnt, int bar_count,
        bool vertical_bars, std::vector<RECT>& out_rects) const;
    bool is_channel_freq_flipped(size_t slot_index, size_t slot_cnt) const;
    void update_transparent_rgn();
    void clear_transparent_rgn();

    void on_paint();
    void on_timer();
    void on_context_menu(int screen_x, int screen_y);
    void sample_spectrum_and_update(double dt_ms);
    void render_to_backbuffer(int w, int h);
    void render_bars_rect(HDC dc, const RECT& plot, const std::vector<int>& chan_indices,
        int ch_pos, int nch_visible, bool vertical_bars, bool invert_dir);

    HWND m_wnd = nullptr;
    uie::window_host_ptr m_host;
    cs_config m_cfg;
    visualisation_stream_v2::ptr m_vis;
    bool m_vis_new_fft = true;

    UINT_PTR m_timer_id = 0;
    ULONGLONG m_last_tick = 0;

    // Offscreen double-buffer, cached gradient & captured parent background
    int m_cache_w = 0;
    int m_cache_h = 0;
    HDC m_back_dc = nullptr;
    HBITMAP m_back_bmp = nullptr;
    HBITMAP m_back_old_bmp = nullptr;
    HDC m_grad_dc = nullptr;
    HBITMAP m_grad_bmp = nullptr;
    HBITMAP m_grad_old_bmp = nullptr;
    HDC m_parent_bg_dc = nullptr;
    HBITMAP m_parent_bg_bmp = nullptr;
    HBITMAP m_parent_bg_old_bmp = nullptr;
    bool m_parent_bg_valid = false;
    bool m_capturing_parent_bg = false;
    std::vector<RECT> m_last_rgn_bands;
    bool m_has_rgn = false;
    bool m_grad_dirty = true;

    // Per-visible-channel bar states
    std::vector<std::vector<bar_state>> m_states;
    audio_chunk_fast_impl m_chunk;
};

bool show_channel_spectrum_config_dialog(HWND parent, channel_spectrum_window* owner, cs_config& cfg);

} // namespace cs
