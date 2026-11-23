#include "channel_spectrum.h"

#include "../foobar2000/SDK/playback_control.h"
#include "../columns_ui-sdk-main/win32_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cs {

namespace {

constexpr wchar_t kClassName[] = L"ChannelSpectrumPanelX64";
constexpr UINT_PTR kVisTimerId = 0x7FFF;

constexpr UINT kCmdConfigure = 1001;
constexpr UINT kCmdColorBase = 1100; // +0..5
constexpr UINT kCmdGridToggle = 1201;
constexpr UINT kCmdPeakToggle = 1202;
constexpr UINT kCmdTransparentToggle = 1203;
constexpr UINT kCmdFastPseudoToggle = 1204;
constexpr UINT kCmdChannelModeBase = 1300; // +0..2
constexpr UINT kCmdChannelDirBase = 1310;  // +0..2

inline COLORREF clamp_rgb(uint32_t v) {
    return static_cast<COLORREF>(v & 0x00FFFFFFu);
}

// Coalesce arbitrary client rects into sorted, disjoint Y-X scanline bands required by ExtCreateRegion.
void build_disjoint_bands(const std::vector<RECT>& raw_rects, int w, int h, std::vector<RECT>& out_bands) {
    out_bands.clear();
    if (w <= 0 || h <= 0 || raw_rects.empty()) return;

    std::vector<RECT> clipped;
    clipped.reserve(raw_rects.size());
    std::vector<int> ys;
    ys.reserve(raw_rects.size() * 2);

    for (const RECT& r : raw_rects) {
        int l = (std::max<int>)(0, r.left);
        int t = (std::max<int>)(0, r.top);
        int ri = (std::min<int>)(w, r.right);
        int b = (std::min<int>)(h, r.bottom);
        if (l < ri && t < b) {
            clipped.push_back(RECT{ l, t, ri, b });
            ys.push_back(t);
            ys.push_back(b);
        }
    }
    if (clipped.empty()) return;

    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    if (ys.size() < 2) return;

    std::vector<std::pair<int, int>> spans;
    std::vector<std::pair<int, int>> prev_spans;
    int prev_y0 = 0;
    int prev_y1 = 0;
    size_t prev_band_start = 0;

    for (size_t yi = 0; yi + 1 < ys.size(); ++yi) {
        int y0 = ys[yi];
        int y1 = ys[yi + 1];
        if (y0 >= y1) continue;

        spans.clear();
        for (const RECT& r : clipped) {
            if (r.top <= y0 && r.bottom >= y1) {
                spans.emplace_back(r.left, r.right);
            }
        }
        if (spans.empty()) {
            prev_spans.clear();
            continue;
        }

        std::sort(spans.begin(), spans.end(), [](const auto& a, const auto& b) {
            return a.first < b.first || (a.first == b.first && a.second < b.second);
        });

        size_t write = 0;
        for (size_t i = 0; i < spans.size(); ++i) {
            if (write == 0 || spans[i].first > spans[write - 1].second) {
                spans[write++] = spans[i];
            } else if (spans[i].second > spans[write - 1].second) {
                spans[write - 1].second = spans[i].second;
            }
        }
        spans.resize(write);

        if (!prev_spans.empty() && prev_y1 == y0 && spans == prev_spans) {
            for (size_t k = prev_band_start; k < out_bands.size(); ++k) {
                out_bands[k].bottom = y1;
            }
            prev_y1 = y1;
        } else {
            prev_band_start = out_bands.size();
            for (const auto& s : spans) {
                out_bands.push_back(RECT{ s.first, y0, s.second, y1 });
            }
            prev_spans = spans;
            prev_y0 = y0;
            prev_y1 = y1;
        }
    }
    (void)prev_y0;
}

HRGN create_region_from_bands(const std::vector<RECT>& bands) {
    if (bands.empty()) {
        return CreateRectRgn(0, 0, 0, 0);
    }
    const size_t data_bytes = sizeof(RGNDATAHEADER) + bands.size() * sizeof(RECT);
    std::vector<uint8_t> buf(data_bytes);
    auto* rd = reinterpret_cast<RGNDATA*>(buf.data());
    rd->rdh.dwSize = sizeof(RGNDATAHEADER);
    rd->rdh.iType = RDH_RECTANGLES;
    rd->rdh.nCount = static_cast<DWORD>(bands.size());
    rd->rdh.nRgnSize = static_cast<DWORD>(bands.size() * sizeof(RECT));

    RECT bound = bands.front();
    for (const RECT& r : bands) {
        if (r.left < bound.left) bound.left = r.left;
        if (r.top < bound.top) bound.top = r.top;
        if (r.right > bound.right) bound.right = r.right;
        if (r.bottom > bound.bottom) bound.bottom = r.bottom;
    }
    rd->rdh.rcBound = bound;
    std::memcpy(rd->Buffer, bands.data(), bands.size() * sizeof(RECT));
    return ExtCreateRegion(nullptr, static_cast<DWORD>(data_bytes), rd);
}

TRIVERTEX make_vertex(LONG x, LONG y, COLORREF c) {
    TRIVERTEX v{};
    v.x = x;
    v.y = y;
    v.Red = static_cast<COLOR16>(GetRValue(c) << 8);
    v.Green = static_cast<COLOR16>(GetGValue(c) << 8);
    v.Blue = static_cast<COLOR16>(GetBValue(c) << 8);
    v.Alpha = 0;
    return v;
}

uint32_t snap_fft_size(uint32_t v) {
    const uint32_t valid[] = { 512, 1024, 2048, 4096, 8192 };
    for (uint32_t c : valid) {
        if (v <= c) return c;
    }
    return 8192;
}

} // namespace

// ---------------------------------------------------------------------------
// cs_config
// ---------------------------------------------------------------------------

cs_config::cs_config() {
    reset_defaults();
}

void cs_config::reset_defaults() {
    raw.fill(0);
    fg1 = RGB(0xe4, 0xe4, 0xe4);
    fg2 = RGB(0x20, 0x20, 0x20);
    bg = RGB(0x00, 0x00, 0x00);
    peak_color = RGB(0xff, 0xff, 0xff);
    ruler_color = RGB(0xf5, 0xf5, 0xf5);
    fg_stops = {
        RGB(0x99, 0x11, 0x11), RGB(0xc6, 0x22, 0x15), RGB(0xce, 0x42, 0x1a), RGB(0xd7, 0x83, 0x11),
        RGB(0xad, 0x9a, 0x16), RGB(0x89, 0x8e, 0x28), RGB(0x31, 0x8c, 0x76), RGB(0x36, 0x64, 0x7a),
    };
    show_peak = true;
    color_mode = ColorVertical;
    border_style = BorderNone;
    bar_width = 10;
    transparent = false;
    fast_pseudo_transparent = false;
    log_amp = true;
    linear_freq = false;
    freq_flip = false;
    refresh_ms = 25;
    fft_size = 2048;
    peak_hold_ms = 100;
    grid_mode = true;
    channel_mode = ChannelSpread;
    channel_single = 0;
    chan_flip = false;
    rotation = 0;
    scale_pct = 100;
    spacing = 0;
    peak_fall_speed = 10;
    blur_level = 25;
    use_custom_fg = false;
    use_custom_bg = false;
    bg_stops.fill(RGB(0x00, 0x00, 0x00));
    sync_scanline = false;
    show_freq_ruler = false;
    show_amp_ruler = false;
    for (uint32_t i = 0; i < 18; ++i) channel_order[i] = i;
    channel_dir = 0;
    channel_mask = 0x3ffffu;
    channel_spacing = 0;
    pad_left = 5;
    pad_right = 5;
    raw[69] = 3;
    raw[72] = 100;
    fft_new = true;
    pack_to_raw();
}

void cs_config::unpack_from_raw() {
    fg1 = clamp_rgb(raw[0]);
    bg = clamp_rgb(raw[1]);
    peak_color = clamp_rgb(raw[2]);
    show_peak = raw[3] != 0;
    color_mode = (raw[4] <= ColorCurve) ? raw[4] : ColorVertical;
    border_style = (raw[5] <= BorderGrey) ? raw[5] : BorderNone;
    bar_width = std::clamp<uint32_t>(raw[6] ? raw[6] : 10, 1, 256);
    transparent = (raw[7] & 1u) != 0;
    fast_pseudo_transparent = (raw[7] & 2u) != 0;
    log_amp = raw[8] != 0;
    fg2 = clamp_rgb(raw[9]);
    refresh_ms = std::clamp<uint32_t>(raw[10] ? raw[10] : 25, 10, 500);
    fft_size = snap_fft_size(raw[11] ? raw[11] : 2048);
    peak_fall_speed = std::clamp<uint32_t>(raw[12] ? raw[12] : 10, 1, 99);
    grid_mode = raw[13] != 0;
    channel_mode = (raw[14] <= ChannelCustom) ? raw[14] : ChannelSpread;
    channel_single = raw[15] % 18u;
    freq_flip = raw[16] != 0;
    rotation = raw[17] & 3u;
    chan_flip = raw[18] != 0;
    scale_pct = std::clamp<uint32_t>(raw[19] ? raw[19] : 100, 1, 1000);
    spacing = std::clamp<uint32_t>(raw[20], 0, 64);
    blur_level = std::clamp<uint32_t>(raw[21] ? raw[21] : 25, 1, 100);
    show_freq_ruler = raw[22] != 0;
    for (size_t i = 0; i < 6; ++i) fg_stops[i + 2] = clamp_rgb(raw[23 + i]);
    use_custom_fg = raw[29] != 0;
    fg_stops[0] = clamp_rgb(raw[30]);
    fg_stops[1] = clamp_rgb(raw[31]);
    linear_freq = raw[32] != 0;
    show_amp_ruler = raw[33] != 0;
    ruler_color = clamp_rgb(raw[34]);
    for (size_t i = 0; i < 8; ++i) bg_stops[i] = clamp_rgb(raw[35 + i]);
    use_custom_bg = raw[43] != 0;
    peak_hold_ms = std::clamp<uint32_t>(raw[44], 0u, 10000u);
    sync_scanline = raw[45] != 0;
    for (size_t i = 0; i < 18; ++i) {
        channel_order[i] = (raw[46 + i] < 18) ? raw[46 + i] : static_cast<uint32_t>(i);
    }
    channel_dir = (raw[64] <= 2u) ? raw[64] : 0u;
    channel_mask = raw[65] ? (raw[65] & 0x3ffffu) : 0x3ffffu;
    channel_spacing = std::clamp<uint32_t>(raw[68], 0u, 99u);
    pad_left = std::clamp<uint32_t>(raw[70], 0, 100);
    pad_right = std::clamp<uint32_t>(raw[71], 0, 100);
    fft_new = raw[73] != 0;
}

void cs_config::pack_to_raw() {
    raw[0] = fg1;
    raw[1] = bg;
    raw[2] = peak_color;
    raw[3] = show_peak ? 1u : 0u;
    raw[4] = color_mode;
    raw[5] = border_style;
    raw[6] = bar_width;
    raw[7] = (transparent ? 1u : 0u) | (fast_pseudo_transparent ? 2u : 0u);
    raw[8] = log_amp ? 1u : 0u;
    raw[9] = fg2;
    raw[10] = refresh_ms;
    raw[11] = fft_size;
    raw[12] = peak_fall_speed;
    raw[13] = grid_mode ? 1u : 0u;
    raw[14] = channel_mode;
    raw[15] = channel_single % 18u;
    raw[16] = freq_flip ? 1u : 0u;
    raw[17] = rotation & 3u;
    raw[18] = chan_flip ? 1u : 0u;
    raw[19] = scale_pct;
    raw[20] = spacing;
    raw[21] = blur_level;
    raw[22] = show_freq_ruler ? 1u : 0u;
    for (size_t i = 0; i < 6; ++i) raw[23 + i] = fg_stops[i + 2];
    raw[29] = use_custom_fg ? 1u : 0u;
    raw[30] = fg_stops[0];
    raw[31] = fg_stops[1];
    raw[32] = linear_freq ? 1u : 0u;
    raw[33] = show_amp_ruler ? 1u : 0u;
    raw[34] = ruler_color;
    for (size_t i = 0; i < 8; ++i) raw[35 + i] = bg_stops[i];
    raw[43] = use_custom_bg ? 1u : 0u;
    raw[44] = peak_hold_ms;
    raw[45] = sync_scanline ? 1u : 0u;
    for (size_t i = 0; i < 18; ++i) raw[46 + i] = channel_order[i];
    raw[64] = channel_dir % 3u;
    raw[65] = channel_mask;
    raw[68] = channel_spacing;
    if (raw[69] == 0) raw[69] = 3;
    raw[70] = pad_left;
    raw[71] = pad_right;
    if (raw[72] == 0) raw[72] = 100;
    raw[73] = fft_new ? 1u : 0u;
}

void cs_config::read_stream(stream_reader* r, t_size size, abort_callback& abort) {
    reset_defaults();
    if (!r || size == 0) return;

    const t_size max_bytes = sizeof(uint32_t) * raw.size();
    const t_size to_read = (std::min)(size, max_bytes);
    if (to_read > 0) {
        r->read_object(raw.data(), to_read, abort);
    }
    if (size > to_read) {
        r->skip_object(size - to_read, abort);
    }
    unpack_from_raw();
}

void cs_config::write_stream(stream_writer* w, abort_callback& abort) const {
    if (!w) return;
    cs_config tmp = *this;
    tmp.pack_to_raw();
    w->write_object(tmp.raw.data(), sizeof(uint32_t) * tmp.raw.size(), abort);
}

// ---------------------------------------------------------------------------
// channel_spectrum_window
// ---------------------------------------------------------------------------

channel_spectrum_window::~channel_spectrum_window() {
    destroy_window();
}

void channel_spectrum_window::set_config(stream_reader* p_reader, t_size p_size, abort_callback& p_abort) {
    m_cfg.read_stream(p_reader, p_size, p_abort);
    m_grad_dirty = true;
    invalidate_parent_bg();
    if (m_wnd && IsWindow(m_wnd)) {
        recreate_vis_stream();
        restart_timer();
        if (effective_transparent_mode() == 1) {
            update_transparent_rgn();
        } else {
            clear_transparent_rgn();
        }
        InvalidateRect(m_wnd, nullptr, FALSE);
    }
}

void channel_spectrum_window::get_config(stream_writer* p_writer, abort_callback& p_abort) const {
    m_cfg.write_stream(p_writer, p_abort);
}

bool channel_spectrum_window::show_config_popup(HWND wnd_parent) {
    cs_config copy = m_cfg;
    if (show_channel_spectrum_config_dialog(wnd_parent, this, copy)) {
        apply_config(copy);
        return true;
    }
    return false;
}

void channel_spectrum_window::apply_config(const cs_config& next) {
    bool stream_changed = (next.fft_new != m_cfg.fft_new);
    bool timer_changed = (next.refresh_ms != m_cfg.refresh_ms);
    m_cfg = next;
    m_cfg.pack_to_raw();
    m_grad_dirty = true;
    invalidate_parent_bg();
    if (stream_changed) recreate_vis_stream();
    if (timer_changed) restart_timer();
    if (m_wnd && IsWindow(m_wnd)) {
        if (effective_transparent_mode() == 1) {
            update_transparent_rgn();
        } else {
            clear_transparent_rgn();
        }
        InvalidateRect(m_wnd, nullptr, FALSE);
    }
}

void channel_spectrum_window::ensure_window_class() {
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &channel_spectrum_window::wnd_proc;
    wc.hInstance = core_api::get_my_instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
}

HWND channel_spectrum_window::create_or_transfer_window(
    HWND wnd_parent, const uie::window_host_ptr& p_host, const ui_helpers::window_position_t& p_position) {
    if (m_wnd && IsWindow(m_wnd)) {
        ShowWindow(m_wnd, SW_HIDE);
        SetParent(m_wnd, wnd_parent);
        SetWindowPos(m_wnd, nullptr, p_position.x, p_position.y, p_position.cx, p_position.cy,
            SWP_NOZORDER | SWP_NOACTIVATE);
        m_host = p_host;
        return m_wnd;
    }

    m_host = p_host;
    ensure_window_class();
    DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    m_wnd = CreateWindowExW(
        0, kClassName, L"", style,
        p_position.x, p_position.y, p_position.cx, p_position.cy,
        wnd_parent, nullptr, core_api::get_my_instance(), this);
    return m_wnd;
}

void channel_spectrum_window::destroy_window() {
    stop_timer();
    m_vis.release();
    destroy_gdi_cache();
    if (m_wnd && IsWindow(m_wnd)) {
        HWND hwnd = m_wnd;
        m_wnd = nullptr;
        DestroyWindow(hwnd);
    }
    m_host.release();
}

void channel_spectrum_window::recreate_vis_stream() {
    if (!m_wnd || !IsWindow(m_wnd)) return;
    if (m_vis.is_valid() && m_vis_new_fft == m_cfg.fft_new) return;
    m_vis.release();
    try {
        unsigned flags = m_cfg.fft_new ? visualisation_manager::KStreamFlagNewFFT : 0;
        visualisation_manager::get()->create_stream(m_vis, flags);
        m_vis_new_fft = m_cfg.fft_new;
    } catch (...) {
        m_vis.release();
    }
}

void channel_spectrum_window::restart_timer() {
    if (!m_wnd || !IsWindow(m_wnd)) return;
    stop_timer();
    UINT ms = std::clamp<UINT>(m_cfg.refresh_ms, 10, 500);
    m_last_tick = GetTickCount64();
    m_timer_id = SetTimer(m_wnd, kVisTimerId, ms, nullptr);
}

void channel_spectrum_window::stop_timer() {
    if (m_wnd && IsWindow(m_wnd) && m_timer_id != 0) {
        KillTimer(m_wnd, m_timer_id);
        m_timer_id = 0;
    }
}

void channel_spectrum_window::destroy_gdi_cache() {
    if (m_parent_bg_dc) {
        if (m_parent_bg_old_bmp) SelectObject(m_parent_bg_dc, m_parent_bg_old_bmp);
        if (m_parent_bg_bmp) DeleteObject(m_parent_bg_bmp);
        DeleteDC(m_parent_bg_dc);
    }
    if (m_grad_dc) {
        if (m_grad_old_bmp) SelectObject(m_grad_dc, m_grad_old_bmp);
        if (m_grad_bmp) DeleteObject(m_grad_bmp);
        DeleteDC(m_grad_dc);
    }
    if (m_back_dc) {
        if (m_back_old_bmp) SelectObject(m_back_dc, m_back_old_bmp);
        if (m_back_bmp) DeleteObject(m_back_bmp);
        DeleteDC(m_back_dc);
    }
    m_parent_bg_dc = nullptr;
    m_parent_bg_bmp = nullptr;
    m_parent_bg_old_bmp = nullptr;
    m_parent_bg_valid = false;
    m_back_dc = nullptr;
    m_back_bmp = nullptr;
    m_back_old_bmp = nullptr;
    m_grad_dc = nullptr;
    m_grad_bmp = nullptr;
    m_grad_old_bmp = nullptr;
    m_cache_w = 0;
    m_cache_h = 0;
    m_grad_dirty = true;
}

void channel_spectrum_window::ensure_gdi_cache(HDC ref_dc, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (m_back_dc && m_cache_w == w && m_cache_h == h) {
        if (m_grad_dirty) rebuild_gradient_bitmap(ref_dc, w, h);
        return;
    }
    destroy_gdi_cache();
    m_back_dc = CreateCompatibleDC(ref_dc);
    m_back_bmp = CreateCompatibleBitmap(ref_dc, w, h);
    m_back_old_bmp = static_cast<HBITMAP>(SelectObject(m_back_dc, m_back_bmp));

    m_grad_dc = CreateCompatibleDC(ref_dc);
    m_grad_bmp = CreateCompatibleBitmap(ref_dc, w, h);
    m_grad_old_bmp = static_cast<HBITMAP>(SelectObject(m_grad_dc, m_grad_bmp));

    m_cache_w = w;
    m_cache_h = h;
    m_grad_dirty = true;
    m_parent_bg_valid = false;
    rebuild_gradient_bitmap(ref_dc, w, h);
}

void channel_spectrum_window::invalidate_parent_bg() {
    m_parent_bg_valid = false;
}

int channel_spectrum_window::effective_transparent_mode() const {
    if (!m_cfg.transparent) return 0;
    if (m_cfg.fast_pseudo_transparent || m_cfg.color_mode == ColorCurve) return 3;
    return 1;
}

void channel_spectrum_window::ensure_parent_bg(HDC ref_dc, int w, int h) {
    if (w <= 0 || h <= 0 || m_capturing_parent_bg) return;
    if (!m_parent_bg_dc) {
        m_parent_bg_dc = CreateCompatibleDC(ref_dc);
        m_parent_bg_bmp = CreateCompatibleBitmap(ref_dc, w, h);
        m_parent_bg_old_bmp = static_cast<HBITMAP>(SelectObject(m_parent_bg_dc, m_parent_bg_bmp));
        m_parent_bg_valid = false;
    }
    if (m_parent_bg_valid) return;

    HWND parent = m_wnd ? GetParent(m_wnd) : nullptr;
    if (!parent || !IsWindow(parent)) {
        RECT full{ 0, 0, w, h };
        HBRUSH br = CreateSolidBrush(m_cfg.bg);
        FillRect(m_parent_bg_dc, &full, br);
        DeleteObject(br);
        m_parent_bg_valid = true;
        return;
    }

    RECT prc_parent{};
    GetClientRect(parent, &prc_parent);
    const int pw = prc_parent.right - prc_parent.left;
    const int ph = prc_parent.bottom - prc_parent.top;

    POINT pt{ 0, 0 };
    MapWindowPoints(m_wnd, parent, &pt, 1);

    m_capturing_parent_bg = true;
    HRGN empty_rgn = CreateRectRgn(0, 0, 0, 0);
    if (empty_rgn) {
        if (!SetWindowRgn(m_wnd, empty_rgn, FALSE)) {
            DeleteObject(empty_rgn);
        }
    }

    bool captured = false;
    if (pw > 0 && ph > 0) {
        HDC full_dc = CreateCompatibleDC(ref_dc);
        HBITMAP full_bmp = CreateCompatibleBitmap(ref_dc, pw, ph);
        if (full_dc && full_bmp) {
            HGDIOBJ old_full = SelectObject(full_dc, full_bmp);
            captured = (PrintWindow(parent, full_dc, 0) != FALSE);
            if (!captured) {
                captured = (SendMessageW(parent, WM_PRINT, reinterpret_cast<WPARAM>(full_dc),
                    PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN) != 0);
            }
            if (captured) {
                captured = (BitBlt(m_parent_bg_dc, 0, 0, w, h, full_dc, pt.x, pt.y, SRCCOPY) != FALSE);
            }
            SelectObject(full_dc, old_full);
        }
        if (full_bmp) DeleteObject(full_bmp);
        if (full_dc) DeleteDC(full_dc);
    }

    if (!captured) {
        RECT prc{ pt.x, pt.y, pt.x + w, pt.y + h };
        RedrawWindow(parent, &prc, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_ALLCHILDREN);
        if (HDC pdc = GetDC(parent)) {
            BitBlt(m_parent_bg_dc, 0, 0, w, h, pdc, pt.x, pt.y, SRCCOPY);
            ReleaseDC(parent, pdc);
        }
    }

    SetWindowRgn(m_wnd, nullptr, FALSE);
    m_has_rgn = false;
    m_last_rgn_bands.clear();
    m_capturing_parent_bg = false;
    m_parent_bg_valid = true;
}

void channel_spectrum_window::clear_transparent_rgn() {
    if (m_has_rgn && m_wnd && IsWindow(m_wnd)) {
        SetWindowRgn(m_wnd, nullptr, TRUE);
    }
    m_has_rgn = false;
    m_last_rgn_bands.clear();
}

bool channel_spectrum_window::is_channel_freq_flipped(size_t slot_index, size_t slot_cnt) const {
    bool base_flip = false;
    if (m_cfg.channel_dir == 0) {
        // Legacy "反射" (Mirror): with 2+ channels, Left draws High->Low (bass at center) and Right Low->High.
        if (slot_cnt >= 2) {
            base_flip = ((slot_index & 1u) == 0);
        }
    } else if (m_cfg.channel_dir == 2) {
        base_flip = true;
    }
    return base_flip != m_cfg.freq_flip;
}

void channel_spectrum_window::compute_channel_slot_rects(
    const RECT& inner, size_t slot_cnt, int bar_count, bool vertical_bars, std::vector<RECT>& out_rects) const {
    out_rects.clear();
    if (slot_cnt == 0) return;

    const int iw = inner.right - inner.left;
    const int ih = inner.bottom - inner.top;
    const int axis_len = vertical_bars ? iw : ih;
    const int chan_gap = static_cast<int>(m_cfg.channel_spacing);
    out_rects.resize(slot_cnt, inner);

    if (m_cfg.color_mode == ColorCurve || (slot_cnt == 1 && bar_count <= 0)) {
        const int avail = (std::max)(1, axis_len - static_cast<int>(slot_cnt - 1) * chan_gap);
        for (size_t c = 0; c < slot_cnt; ++c) {
            RECT r = inner;
            int s0 = (static_cast<int>(c) * avail) / static_cast<int>(slot_cnt) + static_cast<int>(c) * chan_gap;
            int s1 = (static_cast<int>(c + 1) * avail) / static_cast<int>(slot_cnt) + static_cast<int>(c) * chan_gap;
            if (vertical_bars) {
                r.left = inner.left + s0;
                r.right = inner.left + s1;
            } else {
                r.top = inner.top + s0;
                r.bottom = inner.top + s1;
            }
            out_rects[c] = r;
        }
        return;
    }

    const int bar_w = static_cast<int>((std::max)(1u, m_cfg.bar_width));
    const int gap = (m_cfg.grid_mode ? 1 : 0) + static_cast<int>(m_cfg.spacing);
    const int pitch = (std::max)(1, bar_w + gap);
    const int slot_used = bar_count * pitch - gap;
    const int inter_step = slot_used + gap + chan_gap;
    const int total_used = static_cast<int>(slot_cnt) * slot_used + static_cast<int>(slot_cnt - 1) * (gap + chan_gap);
    const int start0 = (std::max)(0, (axis_len - total_used) / 2);

    for (size_t c = 0; c < slot_cnt; ++c) {
        RECT r = inner;
        int s0 = start0 + static_cast<int>(c) * inter_step;
        int s1 = (std::min)(axis_len, s0 + slot_used);
        if (vertical_bars) {
            r.left = inner.left + s0;
            r.right = inner.left + s1;
        } else {
            r.top = inner.top + s0;
            r.bottom = inner.top + s1;
        }
        out_rects[c] = r;
    }
}

void channel_spectrum_window::collect_mode1_bands(int w, int h, std::vector<RECT>& out_bands) const {
    out_bands.clear();
    if (w <= 0 || h <= 0) return;

    std::vector<RECT> raw_rects;
    RECT full{ 0, 0, w, h };
    RECT inner = full;
    if (m_cfg.border_style != BorderNone) {
        raw_rects.push_back(RECT{ 0, 0, w, 1 });
        raw_rects.push_back(RECT{ 0, h - 1, w, h });
        raw_rects.push_back(RECT{ 0, 1, 1, h - 1 });
        raw_rects.push_back(RECT{ w - 1, 1, w, h - 1 });
        InflateRect(&inner, -1, -1);
    }

    const int iw = inner.right - inner.left;
    const int ih = inner.bottom - inner.top;
    if (iw > 0 && ih > 0 && !m_states.empty()) {
        const bool vertical_bars = (m_cfg.rotation == 0 || m_cfg.rotation == 2);
        const bool invert_dir = (m_cfg.rotation == 2 || m_cfg.rotation == 3);
        const int nch = static_cast<int>(m_states.size());
        const size_t slot_cnt = (m_cfg.channel_mode == ChannelSpread && nch > 1) ? static_cast<size_t>(nch) : 1u;
        const int bar_count_ref = static_cast<int>(m_states[0].size());
        std::vector<RECT> slot_rects;
        compute_channel_slot_rects(inner, slot_cnt, bar_count_ref, vertical_bars, slot_rects);

        for (size_t c = 0; c < slot_rects.size(); ++c) {
            const RECT& plot = slot_rects[c];
            const int pw = plot.right - plot.left;
            const int ph = plot.bottom - plot.top;
            if (pw <= 0 || ph <= 0 || c >= m_states.size()) continue;
            const auto& bars = m_states[c];
            if (bars.empty()) continue;

            const int bar_count = static_cast<int>(bars.size());
            const int bar_w = static_cast<int>((std::max)(1u, m_cfg.bar_width));
            const int gap = ((m_cfg.grid_mode && m_cfg.color_mode != ColorCurve) ? 1 : 0)
                + static_cast<int>(m_cfg.spacing);
            const int pitch = (std::max)(1, bar_w + gap);
            const int used = bar_count * pitch - gap;
            const int axis_span = vertical_bars ? pw : ph;
            const int start_off = (std::max)(0, (axis_span - used) / 2);

            for (int b = 0; b < bar_count; ++b) {
                const bar_state& st = bars[b];
                const int pos = start_off + b * pitch;
                if (pos >= axis_span) break;
                const int cur_w = (std::min)(bar_w, axis_span - pos);

                if (vertical_bars) {
                    const int bar_h = static_cast<int>(std::lround(st.level * ph));
                    const int x = plot.left + pos;
                    if (bar_h > 0) {
                        const int top = invert_dir ? plot.top : (plot.bottom - bar_h);
                        const int bot = top + bar_h;
                        if (m_cfg.grid_mode) {
                            for (int yy = bot - 1; yy >= top; yy -= 2) {
                                int y_draw = invert_dir ? (top + (bot - 1 - yy)) : yy;
                                raw_rects.push_back(RECT{ x, y_draw, x + cur_w, y_draw + 1 });
                            }
                        } else {
                            raw_rects.push_back(RECT{ x, top, x + cur_w, bot });
                        }
                    }
                    if (m_cfg.show_peak && st.peak > 0.005) {
                        const int ph_px = static_cast<int>(std::lround(st.peak * ph));
                        int py = invert_dir ? (plot.top + ph_px - 1) : (plot.bottom - ph_px);
                        py = std::clamp<int>(py, plot.top, plot.bottom - 1);
                        raw_rects.push_back(RECT{ x, py, x + cur_w, py + 1 });
                    }
                } else {
                    const int bar_len = static_cast<int>(std::lround(st.level * pw));
                    const int y = plot.top + pos;
                    if (bar_len > 0) {
                        const int left = invert_dir ? (plot.right - bar_len) : plot.left;
                        const int right = left + bar_len;
                        if (m_cfg.grid_mode) {
                            for (int xx = (invert_dir ? right - 1 : left);
                                 invert_dir ? (xx >= left) : (xx < right);
                                 xx += (invert_dir ? -2 : 2)) {
                                raw_rects.push_back(RECT{ xx, y, xx + 1, y + cur_w });
                            }
                        } else {
                            raw_rects.push_back(RECT{ left, y, right, y + cur_w });
                        }
                    }
                    if (m_cfg.show_peak && st.peak > 0.005) {
                        const int plen = static_cast<int>(std::lround(st.peak * pw));
                        int px = invert_dir ? (plot.right - plen) : (plot.left + plen - 1);
                        px = std::clamp<int>(px, plot.left, plot.right - 1);
                        raw_rects.push_back(RECT{ px, y, px + 1, y + cur_w });
                    }
                }
            }
        }
    }

    build_disjoint_bands(raw_rects, w, h, out_bands);
}

void channel_spectrum_window::update_transparent_rgn() {
    if (!m_wnd || !IsWindow(m_wnd) || m_capturing_parent_bg) return;
    if (effective_transparent_mode() != 1) {
        clear_transparent_rgn();
        return;
    }

    RECT rc{};
    GetClientRect(m_wnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    std::vector<RECT> bands;
    collect_mode1_bands(w, h, bands);
    if (m_has_rgn && bands.size() == m_last_rgn_bands.size() &&
        std::equal(bands.begin(), bands.end(), m_last_rgn_bands.begin(),
            [](const RECT& a, const RECT& b) {
                return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
            })) {
        return;
    }

    HRGN hrgn = create_region_from_bands(bands);
    if (hrgn) {
        if (SetWindowRgn(m_wnd, hrgn, TRUE)) {
            m_has_rgn = true;
            m_last_rgn_bands = std::move(bands);
        } else {
            DeleteObject(hrgn);
        }
    }
}

void channel_spectrum_window::rebuild_gradient_bitmap(HDC /*ref_dc*/, int w, int h) {
    if (!m_grad_dc || w <= 0 || h <= 0) return;
    m_grad_dirty = false;

    const bool horiz = (m_cfg.color_mode == ColorHorizontal || m_cfg.color_mode == ColorHorizontalAA);
    const bool simple = (m_cfg.color_mode == ColorSimple || m_cfg.color_mode == ColorSimpleAA);
    if (simple) {
        RECT full{ 0, 0, w, h };
        HBRUSH br = CreateSolidBrush(m_cfg.fg1);
        FillRect(m_grad_dc, &full, br);
        DeleteObject(br);
        return;
    }

    if ((m_cfg.color_mode == ColorVertical || m_cfg.color_mode == ColorHorizontal) && m_cfg.use_custom_fg) {
        constexpr int kBands = 7;
        TRIVERTEX verts[kBands * 2]{};
        GRADIENT_RECT rects[kBands]{};
        for (int i = 0; i < kBands; ++i) {
            if (horiz) {
                int x0 = (i * w) / kBands;
                int x1 = ((i + 1) * w) / kBands;
                verts[i * 2] = make_vertex(x0, 0, m_cfg.fg_stops[i]);
                verts[i * 2 + 1] = make_vertex(x1, h, m_cfg.fg_stops[i + 1]);
            } else {
                int y0 = (i * h) / kBands;
                int y1 = ((i + 1) * h) / kBands;
                verts[i * 2] = make_vertex(0, y0, m_cfg.fg_stops[i]);
                verts[i * 2 + 1] = make_vertex(w, y1, m_cfg.fg_stops[i + 1]);
            }
            rects[i].UpperLeft = static_cast<ULONG>(i * 2);
            rects[i].LowerRight = static_cast<ULONG>(i * 2 + 1);
        }
        GradientFill(m_grad_dc, verts, kBands * 2, rects, kBands,
            horiz ? GRADIENT_FILL_RECT_H : GRADIENT_FILL_RECT_V);
        return;
    }

    TRIVERTEX verts[2]{};
    verts[0] = make_vertex(0, 0, m_cfg.fg1);
    verts[1] = make_vertex(w, h, m_cfg.fg2);
    GRADIENT_RECT grect{ 0, 1 };
    GradientFill(m_grad_dc, verts, 2, &grect, 1,
        horiz ? GRADIENT_FILL_RECT_H : GRADIENT_FILL_RECT_V);
}

LRESULT CALLBACK channel_spectrum_window::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    channel_spectrum_window* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs_ptr = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<channel_spectrum_window*>(cs_ptr->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_wnd = hwnd;
    } else {
        self = reinterpret_cast<channel_spectrum_window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->handle_message(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT channel_spectrum_window::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        recreate_vis_stream();
        restart_timer();
        update_transparent_rgn();
        return 0;

    case WM_DESTROY:
        stop_timer();
        m_vis.release();
        clear_transparent_rgn();
        destroy_gdi_cache();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOVE:
    case WM_WINDOWPOSCHANGED:
        invalidate_parent_bg();
        break;

    case WM_SIZE:
        m_grad_dirty = true;
        invalidate_parent_bg();
        update_transparent_rgn();
        InvalidateRect(m_wnd, nullptr, FALSE);
        return 0;

    case WM_SHOWWINDOW:
        if (wp) {
            invalidate_parent_bg();
            restart_timer();
            update_transparent_rgn();
            InvalidateRect(m_wnd, nullptr, FALSE);
        } else {
            stop_timer();
        }
        return 0;

    case WM_TIMER:
        if (wp == kVisTimerId) {
            on_timer();
            return 0;
        }
        break;

    case WM_PRINT:
    case WM_PRINTCLIENT:
        if (m_capturing_parent_bg) return 0;
        break;

    case WM_PAINT:
        if (m_capturing_parent_bg) {
            ValidateRect(m_wnd, nullptr);
            return 0;
        }
        on_paint();
        return 0;

    case WM_LBUTTONDBLCLK:
        show_config_popup(m_wnd);
        return 0;

    case WM_RBUTTONUP: {
        POINT pt{ static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)) };
        ClientToScreen(m_wnd, &pt);
        on_context_menu(pt.x, pt.y);
        return 0;
    }
    }
    return DefWindowProcW(m_wnd, msg, wp, lp);
}

void channel_spectrum_window::on_context_menu(int screen_x, int screen_y) {
    HMENU menu = CreatePopupMenu();
    HMENU color_menu = CreatePopupMenu();
    HMENU chan_mode_menu = CreatePopupMenu();
    HMENU chan_dir_menu = CreatePopupMenu();
    if (!menu || !color_menu || !chan_mode_menu || !chan_dir_menu) {
        if (menu) DestroyMenu(menu);
        if (color_menu) DestroyMenu(color_menu);
        if (chan_mode_menu) DestroyMenu(chan_mode_menu);
        if (chan_dir_menu) DestroyMenu(chan_dir_menu);
        return;
    }

    const wchar_t* color_names[6] = {
        L"简单", L"水平", L"垂直", L"简单-反锯齿", L"水平-反锯齿", L"曲线"
    };
    for (UINT i = 0; i < 6; ++i) {
        AppendMenuW(color_menu, MF_STRING | (m_cfg.color_mode == i ? MF_CHECKED : 0),
            kCmdColorBase + i, color_names[i]);
    }
    const wchar_t* chan_mode_names[3] = { L"分散", L"集中 (联合)", L"按下面指定" };
    for (UINT i = 0; i < 3; ++i) {
        AppendMenuW(chan_mode_menu, MF_STRING | (m_cfg.channel_mode == i ? MF_CHECKED : 0),
            kCmdChannelModeBase + i, chan_mode_names[i]);
    }
    const wchar_t* chan_dir_names[3] = { L"反射", L"右", L"左" };
    for (UINT i = 0; i < 3; ++i) {
        AppendMenuW(chan_dir_menu, MF_STRING | (m_cfg.channel_dir == i ? MF_CHECKED : 0),
            kCmdChannelDirBase + i, chan_dir_names[i]);
    }

    AppendMenuW(menu, MF_STRING, kCmdConfigure, L"配置...");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(color_menu), L"颜色模式");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(chan_mode_menu), L"声道模式");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(chan_dir_menu), L"声道方向");
    AppendMenuW(menu, MF_STRING | (m_cfg.grid_mode ? MF_CHECKED : 0), kCmdGridToggle, L"栅格模式");
    AppendMenuW(menu, MF_STRING | (m_cfg.show_peak ? MF_CHECKED : 0), kCmdPeakToggle, L"显示波峰");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (m_cfg.transparent ? MF_CHECKED : 0), kCmdTransparentToggle, L"透明背景");
    AppendMenuW(menu,
        MF_STRING | (m_cfg.fast_pseudo_transparent ? MF_CHECKED : 0) | (m_cfg.transparent ? 0 : MF_GRAYED),
        kCmdFastPseudoToggle, L"快速仿透明模式");

    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_x, screen_y, 0, m_wnd, nullptr);
    DestroyMenu(menu); // owns submenus

    if (cmd == kCmdConfigure) {
        show_config_popup(m_wnd);
    } else if (cmd >= kCmdColorBase && cmd < kCmdColorBase + 6) {
        cs_config next = m_cfg;
        next.color_mode = cmd - kCmdColorBase;
        apply_config(next);
    } else if (cmd >= kCmdChannelModeBase && cmd < kCmdChannelModeBase + 3) {
        cs_config next = m_cfg;
        next.channel_mode = cmd - kCmdChannelModeBase;
        apply_config(next);
    } else if (cmd >= kCmdChannelDirBase && cmd < kCmdChannelDirBase + 3) {
        cs_config next = m_cfg;
        next.channel_dir = cmd - kCmdChannelDirBase;
        apply_config(next);
    } else if (cmd == kCmdGridToggle) {
        cs_config next = m_cfg;
        next.grid_mode = !next.grid_mode;
        apply_config(next);
    } else if (cmd == kCmdPeakToggle) {
        cs_config next = m_cfg;
        next.show_peak = !next.show_peak;
        apply_config(next);
    } else if (cmd == kCmdTransparentToggle) {
        cs_config next = m_cfg;
        next.transparent = !next.transparent;
        apply_config(next);
    } else if (cmd == kCmdFastPseudoToggle) {
        cs_config next = m_cfg;
        next.fast_pseudo_transparent = !next.fast_pseudo_transparent;
        apply_config(next);
    }
}

void channel_spectrum_window::on_timer() {
    if (!m_wnd || !IsWindowVisible(m_wnd)) return;
    ULONGLONG now = GetTickCount64();
    double dt_ms = (m_last_tick > 0 && now >= m_last_tick)
        ? static_cast<double>(now - m_last_tick)
        : static_cast<double>(m_cfg.refresh_ms);
    m_last_tick = now;

    sample_spectrum_and_update(std::clamp(dt_ms, 5.0, 200.0));
    if (effective_transparent_mode() == 1) {
        update_transparent_rgn();
    }
    InvalidateRect(m_wnd, nullptr, FALSE);
}

void channel_spectrum_window::on_paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(m_wnd, &ps);
    RECT rc{};
    GetClientRect(m_wnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w > 0 && h > 0) {
        ensure_gdi_cache(dc, w, h);
        if (effective_transparent_mode() == 3) {
            ensure_parent_bg(dc, w, h);
        }
        render_to_backbuffer(w, h);
        BitBlt(dc, 0, 0, w, h, m_back_dc, 0, 0, SRCCOPY);
    }
    EndPaint(m_wnd, &ps);
}

void channel_spectrum_window::sample_spectrum_and_update(double dt_ms) {
    RECT rc{};
    GetClientRect(m_wnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    int pad = (m_cfg.border_style == BorderNone) ? 0 : 1;
    int pw = (std::max)(1, w - pad * 2);
    int ph = (std::max)(1, h - pad * 2);
    const bool vertical_bars = (m_cfg.rotation == 0 || m_cfg.rotation == 2);
    int axis_len = vertical_bars ? pw : ph;

    // Inspect active stream
    bool paused = false;
    try {
        paused = playback_control::get()->is_paused();
    } catch (...) {}

    bool have_spectrum = false;
    if (m_vis.is_valid()) {
        double abs_t = 0.0;
        if (m_vis->get_absolute_time(abs_t)) {
            if (m_vis->get_spectrum_absolute(m_chunk, abs_t, m_cfg.fft_size) && !m_chunk.is_empty()) {
                have_spectrum = true;
            }
        }
    }

    unsigned stream_ch = have_spectrum ? m_chunk.get_channels() : 2u;
    unsigned stream_cfg = have_spectrum ? m_chunk.get_channel_config() : audio_chunk::channel_config_stereo;
    if (stream_ch == 0) stream_ch = 2u;

    // Collect ordered visible channel indices matching channel_mode / channel_mask
    std::vector<int> vis_channels;
    if (m_cfg.channel_mode == ChannelCustom) {
        uint32_t bit = 1u << (m_cfg.channel_single % 18u);
        if (stream_cfg & bit) {
            unsigned idx = audio_chunk::g_channel_index_from_flag(stream_cfg, bit);
            if (idx < stream_ch) vis_channels.push_back(static_cast<int>(idx));
        }
    } else {
        for (uint32_t i = 0; i < 18; ++i) {
            uint32_t bit = 1u << m_cfg.channel_order[i];
            if ((m_cfg.channel_mask & bit) && (stream_cfg & bit)) {
                unsigned idx = audio_chunk::g_channel_index_from_flag(stream_cfg, bit);
                if (idx < stream_ch) vis_channels.push_back(static_cast<int>(idx));
            }
        }
    }
    if (vis_channels.empty()) {
        for (unsigned c = 0; c < (std::min)(stream_ch, 2u); ++c) {
            vis_channels.push_back(static_cast<int>(c));
        }
    }
    if (m_cfg.chan_flip && m_cfg.channel_mode == ChannelSpread) {
        std::reverse(vis_channels.begin(), vis_channels.end());
    }

    const size_t nch = (m_cfg.channel_mode == ChannelGrouped) ? 1u : vis_channels.size();
    const int bar_w = static_cast<int>((std::max)(1u, m_cfg.bar_width));
    const int gap = ((m_cfg.grid_mode && m_cfg.color_mode != ColorCurve) ? 1 : 0)
        + static_cast<int>(m_cfg.spacing);
    const int pitch = (std::max)(1, bar_w + gap);

    int slot_len = (m_cfg.channel_mode == ChannelSpread && nch > 1)
        ? (std::max)(1, (axis_len - static_cast<int>(nch - 1) * static_cast<int>(m_cfg.channel_spacing)) / static_cast<int>(nch))
        : axis_len;
    int bar_count = (m_cfg.color_mode == ColorCurve)
        ? (std::max)(2, slot_len)
        : (std::max)(1, (slot_len + gap) / pitch);

    if (m_states.size() != nch) m_states.resize(nch);
    for (auto& ch_bars : m_states) {
        if (static_cast<int>(ch_bars.size()) != bar_count) {
            ch_bars.assign(bar_count, bar_state{});
        }
    }

    if (paused && have_spectrum) {
        return; // keep frozen bars/peaks while paused
    }

    const int num_bins = have_spectrum ? static_cast<int>(m_chunk.get_sample_count()) : 0;
    const audio_sample* spec_data = have_spectrum ? m_chunk.get_data() : nullptr;

    // Bar smoothing & peak decay parameters matching 32-bit Channel Spectrum
    const double decay_step = (dt_ms / 25.0) * 0.08;
    const double peak_fall_step = (dt_ms / 1000.0) * (static_cast<double>(m_cfg.peak_fall_speed) / 40.0);
    const double peak_hold_ms = static_cast<double>(m_cfg.peak_hold_ms);
    const double scale_mul = static_cast<double>(m_cfg.scale_pct) / 100.0;
    const double lin_k = 2.0 * (std::log2(static_cast<double>(m_cfg.fft_size) * 0.5 + 1.0) * 0.2309);
    const double lin_denom = std::log1p(lin_k);

    for (size_t ci = 0; ci < nch; ++ci) {
        auto& bars = m_states[ci];
        const bool ch_flipped = is_channel_freq_flipped(ci, nch);

        for (int b = 0; b < bar_count; ++b) {
            double target = 0.0;
            if (have_spectrum && num_bins > 2) {
                int eff_b = ch_flipped ? (bar_count - 1 - b) : b;
                int b0 = 0;
                int b1 = 0;
                if (m_cfg.linear_freq) {
                    b0 = 1 + (eff_b * (num_bins - 1)) / bar_count;
                    b1 = 1 + ((eff_b + 1) * (num_bins - 1)) / bar_count;
                } else {
                    double lo = std::pow(static_cast<double>(num_bins - 1),
                        static_cast<double>(eff_b) / static_cast<double>(bar_count));
                    double hi = std::pow(static_cast<double>(num_bins - 1),
                        static_cast<double>(eff_b + 1) / static_cast<double>(bar_count));
                    b0 = static_cast<int>(std::floor(lo));
                    b1 = static_cast<int>(std::floor(hi));
                }
                b0 = std::clamp(b0, 1, num_bins - 1);
                b1 = std::clamp((std::max)(b1, b0 + 1), b0 + 1, num_bins);

                double mag = 0.0;
                if (m_cfg.channel_mode == ChannelGrouped) {
                    for (int ch_idx : vis_channels) {
                        for (int bin = b0; bin < b1; ++bin) {
                            double v = static_cast<double>(spec_data[static_cast<size_t>(bin) * stream_ch + ch_idx]);
                            if (v > mag) mag = v;
                        }
                    }
                } else {
                    int ch_idx = vis_channels[ci];
                    for (int bin = b0; bin < b1; ++bin) {
                        double v = static_cast<double>(spec_data[static_cast<size_t>(bin) * stream_ch + ch_idx]);
                        if (v > mag) mag = v;
                    }
                }

                if (m_cfg.log_amp) {
                    constexpr double kFloor = 1.0 / 131072.0; // 2^-17
                    double norm = 1.0 + (2.0 / 7.0) * std::log10((std::max)(mag, kFloor));
                    target = std::clamp(norm * scale_mul, 0.0, 1.0);
                } else if (m_cfg.fft_new && lin_denom > 0.0) {
                    double norm = std::log1p(lin_k * (std::max)(mag, 0.0)) / lin_denom;
                    target = std::clamp(norm * scale_mul, 0.0, 1.0);
                } else {
                    target = std::clamp(mag * 4.0 * scale_mul, 0.0, 1.0);
                }
            }

            bar_state& st = bars[b];
            if (target >= st.level) {
                st.level = target;
            } else {
                st.level = (std::max)(target, st.level - decay_step);
            }

            if (st.level >= st.peak) {
                st.peak = st.level;
                st.peak_hold_ms = 0.0;
            } else {
                st.peak_hold_ms += dt_ms;
                if (st.peak_hold_ms >= peak_hold_ms) {
                    st.peak = (std::max)(st.level, st.peak - peak_fall_step);
                }
            }
        }
    }
}

void channel_spectrum_window::render_bars_rect(
    HDC dc, const RECT& plot, const std::vector<int>& chan_indices,
    int ch_pos, int nch_visible, bool vertical_bars, bool invert_dir) {
    if (chan_indices.empty() || ch_pos >= static_cast<int>(m_states.size())) return;
    const auto& bars = m_states[ch_pos];
    if (bars.empty()) return;

    const int pw = plot.right - plot.left;
    const int ph = plot.bottom - plot.top;
    if (pw <= 0 || ph <= 0) return;

    const int bar_count = static_cast<int>(bars.size());
    const int bar_w = static_cast<int>((std::max)(1u, m_cfg.bar_width));
    const int gap = ((m_cfg.grid_mode && m_cfg.color_mode != ColorCurve) ? 1 : 0)
        + static_cast<int>(m_cfg.spacing);
    const int pitch = (std::max)(1, bar_w + gap);

    if (m_cfg.color_mode == ColorCurve && vertical_bars) {
        HPEN pen = CreatePen(PS_SOLID, 1, m_cfg.fg1);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        std::vector<POINT> pts(bar_count);
        for (int b = 0; b < bar_count; ++b) {
            int x = plot.left + (b * pw) / (std::max)(1, bar_count - 1);
            int val_h = static_cast<int>(std::lround(bars[b].level * ph));
            int y = invert_dir ? (plot.top + val_h) : (plot.bottom - val_h);
            pts[b] = POINT{ x, std::clamp<LONG>(y, plot.top, plot.bottom - 1) };
        }
        if (bar_count >= 2) Polyline(dc, pts.data(), bar_count);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        return;
    }

    HBRUSH peak_br = m_cfg.show_peak ? CreateSolidBrush(m_cfg.peak_color) : nullptr;

    // Center bars slightly when padding leaves remainder pixels
    int used = bar_count * pitch - gap;
    int axis_span = vertical_bars ? pw : ph;
    int start_off = (std::max)(0, (axis_span - used) / 2);

    for (int b = 0; b < bar_count; ++b) {
        const bar_state& st = bars[b];
        int pos = start_off + b * pitch;
        if (pos >= axis_span) break;
        int cur_w = (std::min)(bar_w, axis_span - pos);

        if (vertical_bars) {
            int bar_h = static_cast<int>(std::lround(st.level * ph));
            int x = plot.left + pos;
            if (bar_h > 0) {
                int top = invert_dir ? plot.top : (plot.bottom - bar_h);
                int bot = top + bar_h;
                if (m_cfg.grid_mode) {
                    for (int yy = bot - 1; yy >= top; yy -= 2) {
                        int y_draw = invert_dir ? (top + (bot - 1 - yy)) : yy;
                        BitBlt(dc, x, y_draw, cur_w, 1, m_grad_dc, x, y_draw, SRCCOPY);
                    }
                } else {
                    BitBlt(dc, x, top, cur_w, bar_h, m_grad_dc, x, top, SRCCOPY);
                }
            }
            if (peak_br && st.peak > 0.005) {
                int ph_px = static_cast<int>(std::lround(st.peak * ph));
                int py = invert_dir ? (plot.top + ph_px - 1) : (plot.bottom - ph_px);
                py = std::clamp<LONG>(py, plot.top, plot.bottom - 1);
                RECT pr{ x, py, x + cur_w, py + 1 };
                FillRect(dc, &pr, peak_br);
            }
        } else {
            int bar_len = static_cast<int>(std::lround(st.level * pw));
            int y = plot.top + pos;
            if (bar_len > 0) {
                int left = invert_dir ? (plot.right - bar_len) : plot.left;
                int right = left + bar_len;
                if (m_cfg.grid_mode) {
                    for (int xx = (invert_dir ? right - 1 : left);
                         invert_dir ? (xx >= left) : (xx < right);
                         xx += (invert_dir ? -2 : 2)) {
                        BitBlt(dc, xx, y, 1, cur_w, m_grad_dc, xx, y, SRCCOPY);
                    }
                } else {
                    BitBlt(dc, left, y, bar_len, cur_w, m_grad_dc, left, y, SRCCOPY);
                }
            }
            if (peak_br && st.peak > 0.005) {
                int plen = static_cast<int>(std::lround(st.peak * pw));
                int px = invert_dir ? (plot.right - plen) : (plot.left + plen - 1);
                px = std::clamp<LONG>(px, plot.left, plot.right - 1);
                RECT pr{ px, y, px + 1, y + cur_w };
                FillRect(dc, &pr, peak_br);
            }
        }
    }

    if (peak_br) DeleteObject(peak_br);
    (void)nch_visible;
}

void channel_spectrum_window::render_to_backbuffer(int w, int h) {
    RECT full{ 0, 0, w, h };
    const int tmode = effective_transparent_mode();
    if (tmode == 3 && m_parent_bg_dc && m_parent_bg_valid) {
        BitBlt(m_back_dc, 0, 0, w, h, m_parent_bg_dc, 0, 0, SRCCOPY);
    } else if (tmode == 0 && m_cfg.use_custom_bg) {
        constexpr int kBands = 7;
        TRIVERTEX verts[kBands * 2]{};
        GRADIENT_RECT rects[kBands]{};
        for (int i = 0; i < kBands; ++i) {
            int y0 = (i * h) / kBands;
            int y1 = ((i + 1) * h) / kBands;
            verts[i * 2] = make_vertex(0, y0, m_cfg.bg_stops[i]);
            verts[i * 2 + 1] = make_vertex(w, y1, m_cfg.bg_stops[i + 1]);
            rects[i].UpperLeft = static_cast<ULONG>(i * 2);
            rects[i].LowerRight = static_cast<ULONG>(i * 2 + 1);
        }
        GradientFill(m_back_dc, verts, kBands * 2, rects, kBands, GRADIENT_FILL_RECT_V);
    } else {
        HBRUSH bg_br = CreateSolidBrush(m_cfg.bg);
        FillRect(m_back_dc, &full, bg_br);
        DeleteObject(bg_br);
    }

    RECT inner = full;
    if (m_cfg.border_style == BorderSunken) {
        DrawEdge(m_back_dc, &inner, BDR_SUNKENOUTER, BF_RECT | BF_ADJUST);
    } else if (m_cfg.border_style == BorderGrey) {
        HBRUSH border_br = CreateSolidBrush(RGB(128, 128, 128));
        FrameRect(m_back_dc, &inner, border_br);
        DeleteObject(border_br);
        InflateRect(&inner, -1, -1);
    }

    int iw = inner.right - inner.left;
    int ih = inner.bottom - inner.top;
    if (iw <= 0 || ih <= 0 || m_states.empty()) return;

    const bool vertical_bars = (m_cfg.rotation == 0 || m_cfg.rotation == 2);
    const bool invert_dir = (m_cfg.rotation == 2 || m_cfg.rotation == 3);
    const int nch = static_cast<int>(m_states.size());
    const size_t slot_cnt = (m_cfg.channel_mode == ChannelSpread && nch > 1) ? static_cast<size_t>(nch) : 1u;
    const int bar_count_ref = static_cast<int>(m_states[0].size());
    std::vector<int> dummy_indices(nch, 0);
    std::vector<RECT> slot_rects;
    compute_channel_slot_rects(inner, slot_cnt, bar_count_ref, vertical_bars, slot_rects);

    for (size_t c = 0; c < slot_rects.size(); ++c) {
        render_bars_rect(m_back_dc, slot_rects[c], dummy_indices, static_cast<int>(c), nch, vertical_bars, invert_dir);
    }
}

static uie::window_factory<channel_spectrum_window> g_channel_spectrum_factory;

} // namespace cs
