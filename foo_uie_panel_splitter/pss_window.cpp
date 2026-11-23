#include "pss_window.h"
#include "pss_expr.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pss {

namespace {

constexpr wchar_t kPssClassName[] = L"PSSWindowContainer";

// {A8F3219C-94E2-4B7A-9C1D-7E6B24F15501}
constexpr GUID kPssHostGuid = {
    0xa8f3219c, 0x94e2, 0x4b7a, {0x9c, 0x1d, 0x7e, 0x6b, 0x24, 0xf1, 0x55, 0x01}
};

// Default 120-byte header tail (24 zeros + Segoe UI -12 LOGFONTW at +24, padded to 120)
void init_default_hdr_tail(pfc::array_t<t_uint8>& out) {
    out.set_size(120);
    std::memset(out.get_ptr(), 0, 120);
    LOGFONTW lf{};
    lf.lfHeight = -12;
    lf.lfWeight = FW_NORMAL;
    lf.lfQuality = PROOF_QUALITY;
    wcsncpy_s(lf.lfFaceName, L"Segoe UI", _TRUNCATE);
    std::memcpy(out.get_ptr() + 24, &lf, (std::min)(sizeof(lf), size_t(96)));
}

void read_len_string(const uint8_t* buf, size_t len, size_t& off, std::string& out) {
    out.clear();
    if (off + 4 > len) {
        off = len;
        return;
    }
    uint32_t slen = 0;
    std::memcpy(&slen, buf + off, 4);
    off += 4;
    if (slen > len - off) slen = static_cast<uint32_t>(len - off);
    if (slen > 0) {
        out.assign(reinterpret_cast<const char*>(buf + off), slen);
        off += slen;
    }
}

void write_len_string(stream_writer* w, const std::string& s, abort_callback& p_abort) {
    uint32_t slen = static_cast<uint32_t>(s.size());
    w->write_lendian_t(slen, p_abort);
    if (slen > 0) w->write(s.data(), slen, p_abort);
}

void ensure_clipsiblings(HWND wnd) {
    if (!wnd || !IsWindow(wnd)) return;
    LONG_PTR st = GetWindowLongPtrW(wnd, GWL_STYLE);
    if ((st & WS_CLIPSIBLINGS) == 0) {
        SetWindowLongPtrW(wnd, GWL_STYLE, st | WS_CLIPSIBLINGS);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// pss_window_host
// ---------------------------------------------------------------------------

const GUID& pss_window_host::get_host_guid() const {
    return kPssHostGuid;
}

void pss_window_host::on_size_limit_change(HWND /*wnd*/, unsigned /*flags*/) {
    if (m_owner) m_owner->relayout();
}

unsigned pss_window_host::is_resize_supported(HWND /*wnd*/) const {
    return 0;
}

bool pss_window_host::request_resize(HWND /*wnd*/, unsigned /*flags*/, unsigned /*width*/, unsigned /*height*/) {
    return false;
}

bool pss_window_host::override_status_text_create(service_ptr_t<ui_status_text_override>& p_out) {
    if (m_owner && m_owner->parent_host().is_valid()) {
        if (m_owner->parent_host()->override_status_text_create(p_out)) return true;
    }
    try {
        return ui_control::get()->override_status_text_create(p_out);
    } catch (...) {
        return false;
    }
}

bool pss_window_host::get_keyboard_shortcuts_enabled() const {
    if (m_owner && m_owner->parent_host().is_valid()) {
        return m_owner->parent_host()->get_keyboard_shortcuts_enabled();
    }
    return true;
}

bool pss_window_host::is_visible(HWND wnd) const {
    if (!wnd || !IsWindow(wnd)) return false;
    return (GetWindowLongPtrW(wnd, GWL_STYLE) & WS_VISIBLE) != 0;
}

bool pss_window_host::is_visibility_modifiable(HWND wnd, bool /*desired_visibility*/) const {
    return wnd != nullptr && IsWindow(wnd);
}

bool pss_window_host::set_window_visibility(HWND wnd, bool visibility) {
    if (!wnd || !IsWindow(wnd)) return false;
    ShowWindow(wnd, visibility ? SW_SHOWNA : SW_HIDE);
    if (m_owner) m_owner->on_child_visibility_changed(wnd);
    return true;
}

void pss_window_host::relinquish_ownership(HWND wnd) {
    if (m_owner) m_owner->on_child_relinquished(wnd);
}

void pss_window_host::get_children(pfc::list_base_t<uie::window_ptr>& p_out) {
    if (m_owner) m_owner->collect_children(p_out);
}

// ---------------------------------------------------------------------------
// panel_stack_splitter
// ---------------------------------------------------------------------------

panel_stack_splitter::panel_stack_splitter() {
    m_child_host = new service_impl_t<pss_window_host>();
    m_child_host->set_owner(this);
    reset_defaults();
}

panel_stack_splitter::~panel_stack_splitter() {
    if (m_child_host.is_valid()) {
        m_child_host->set_owner(nullptr);
    }
    destroy_children();
    if (m_bg_brush) {
        DeleteObject(m_bg_brush);
        m_bg_brush = nullptr;
    }
}

void panel_stack_splitter::reset_defaults() {
    m_hdr_ints[0] = 0;
    m_hdr_ints[1] = 1;
    m_hdr_ints[2] = 0;
    m_bg_color = RGB(40, 40, 40);
    m_hdr_ints2[0] = 0;
    m_hdr_ints2[1] = 0;
    m_script.clear();
    init_default_hdr_tail(m_hdr_tail);
    m_children.clear();
}

void panel_stack_splitter::ensure_window_class() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = core_api::get_my_instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPssClassName;
    if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
}

void panel_stack_splitter::set_config(stream_reader* p_reader, t_size p_size, abort_callback& p_abort) {
    reset_defaults();
    if (!p_reader || p_size == 0) return;

    pfc::array_t<t_uint8> raw;
    raw.set_size(p_size);
    p_reader->read(raw.get_ptr(), p_size, p_abort);

    const uint8_t* buf = raw.get_ptr();
    size_t off = 0;
    if (off + 4 > p_size) return;

    uint32_t hdr_total = 0;
    std::memcpy(&hdr_total, buf + off, 4);
    off += 4;
    if (hdr_total > p_size - off) return;

    const size_t hdr_end = off + hdr_total;
    if (off + 28 <= hdr_end) {
        std::memcpy(m_hdr_ints, buf + off, 12);
        std::memcpy(&m_bg_color, buf + off + 12, 4);
        std::memcpy(m_hdr_ints2, buf + off + 16, 8);
        off += 24;
        read_len_string(buf, hdr_end, off, m_script);
        size_t tail_sz = hdr_end - off;
        if (tail_sz > 0) {
            m_hdr_tail.set_size(tail_sz);
            std::memcpy(m_hdr_tail.get_ptr(), buf + off, tail_sz);
        }
    }
    off = hdr_end;

    if (off + 4 > p_size) return;
    uint32_t child_count = 0;
    std::memcpy(&child_count, buf + off, 4);
    off += 4;
    if (child_count > 512) return;

    m_children.reserve(child_count);
    for (uint32_t i = 0; i < child_count; ++i) {
        if (off + 24 > p_size) break;
        pss_child_entry e;
        std::memcpy(&e.panel_guid, buf + off, 16);
        off += 16;

        read_len_string(buf, p_size, off, e.name);
        if (off + 4 > p_size) break;

        uint32_t meta_len = 0;
        std::memcpy(&meta_len, buf + off, 4);
        off += 4;
        if (meta_len > p_size - off) break;

        const size_t meta_end = off + meta_len;
        if (meta_len >= 68) {
            std::memcpy(&e.ver, buf + off, 4);
            std::memcpy(&e.forced_layout, buf + off + 4, 4);
            std::memcpy(&e.keep_aspect, buf + off + 8, 4);
            std::memcpy(&e.saved_rect, buf + off + 12, 16);
            std::memcpy(e.mid_ints, buf + off + 28, 32);
            std::memcpy(&e.weight, buf + off + 60, 8);
            size_t moff = off + 68;
            read_len_string(buf, meta_end, moff, e.expr_x);
            read_len_string(buf, meta_end, moff, e.expr_y);
            read_len_string(buf, meta_end, moff, e.expr_w);
            read_len_string(buf, meta_end, moff, e.expr_h);
            if (moff + 16 <= meta_end) {
                std::memcpy(e.meta_pad, buf + moff, 12);
                std::memcpy(&e.hidden_on_start, buf + moff + 12, 4);
            } else if (meta_end - off >= 4) {
                std::memcpy(&e.hidden_on_start, buf + meta_end - 4, 4);
            }
        }
        off = meta_end;

        if (off + 4 > p_size) break;
        uint32_t cfg_len = 0;
        std::memcpy(&cfg_len, buf + off, 4);
        off += 4;
        if (cfg_len > p_size - off) cfg_len = static_cast<uint32_t>(p_size - off);
        if (cfg_len > 0) {
            e.cfg_data.set_size(cfg_len);
            std::memcpy(e.cfg_data.get_ptr(), buf + off, cfg_len);
            off += cfg_len;
        }
        m_children.push_back(std::move(e));
    }
}

void panel_stack_splitter::refresh_live_configs() const {
    abort_callback_dummy dummy_abort;
    for (auto& e : m_children) {
        if (e.window.is_valid()) {
            try {
                pfc::array_t<t_uint8> live;
                e.window->get_config_to_array(live, dummy_abort);
                e.cfg_data = live;
            } catch (...) {
            }
        }
        if (e.child_hwnd && IsWindow(e.child_hwnd) && m_wnd && IsWindow(m_wnd)) {
            RECT r{};
            GetWindowRect(e.child_hwnd, &r);
            MapWindowPoints(HWND_DESKTOP, m_wnd, reinterpret_cast<LPPOINT>(&r), 2);
            e.saved_rect = r;
        }
    }
}

void panel_stack_splitter::get_config(stream_writer* p_writer, abort_callback& p_abort) const {
    if (!p_writer) return;
    refresh_live_configs();

    uint32_t hdr_total = static_cast<uint32_t>(28 + m_script.size() + m_hdr_tail.get_size());
    p_writer->write_lendian_t(hdr_total, p_abort);
    p_writer->write(m_hdr_ints, 12, p_abort);
    p_writer->write(&m_bg_color, 4, p_abort);
    p_writer->write(m_hdr_ints2, 8, p_abort);
    write_len_string(p_writer, m_script, p_abort);
    if (m_hdr_tail.get_size() > 0) {
        p_writer->write(m_hdr_tail.get_ptr(), m_hdr_tail.get_size(), p_abort);
    }

    uint32_t count = static_cast<uint32_t>(m_children.size());
    p_writer->write_lendian_t(count, p_abort);
    for (const auto& e : m_children) {
        p_writer->write(&e.panel_guid, 16, p_abort);
        write_len_string(p_writer, e.name, p_abort);

        uint32_t meta_len = static_cast<uint32_t>(
            68 + (4 + e.expr_x.size()) + (4 + e.expr_y.size()) +
            (4 + e.expr_w.size()) + (4 + e.expr_h.size()) + 16);
        p_writer->write_lendian_t(meta_len, p_abort);
        p_writer->write_lendian_t(e.ver, p_abort);
        p_writer->write_lendian_t(e.forced_layout, p_abort);
        p_writer->write_lendian_t(e.keep_aspect, p_abort);
        p_writer->write(&e.saved_rect, 16, p_abort);
        p_writer->write(e.mid_ints, 32, p_abort);
        p_writer->write(&e.weight, 8, p_abort);
        write_len_string(p_writer, e.expr_x, p_abort);
        write_len_string(p_writer, e.expr_y, p_abort);
        write_len_string(p_writer, e.expr_w, p_abort);
        write_len_string(p_writer, e.expr_h, p_abort);
        p_writer->write(e.meta_pad, 12, p_abort);
        p_writer->write_lendian_t(e.hidden_on_start, p_abort);

        uint32_t cfg_len = static_cast<uint32_t>(e.cfg_data.get_size());
        p_writer->write_lendian_t(cfg_len, p_abort);
        if (cfg_len > 0) {
            p_writer->write(e.cfg_data.get_ptr(), cfg_len, p_abort);
        }
    }
}

HWND panel_stack_splitter::create_or_transfer_window(
    HWND wnd_parent, const uie::window_host_ptr& p_host, const ui_helpers::window_position_t& p_position) {
    if (m_wnd && IsWindow(m_wnd)) {
        ShowWindow(m_wnd, SW_HIDE);
        SetParent(m_wnd, wnd_parent);
        if (m_host.is_valid()) m_host->relinquish_ownership(m_wnd);
        m_host = p_host;
        SetWindowPos(m_wnd, nullptr, p_position.x, p_position.y, p_position.cx, p_position.cy,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return m_wnd;
    }

    ensure_window_class();
    m_host = p_host;
    m_wnd = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kPssClassName,
        L"Panel Stack Splitter",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        p_position.x,
        p_position.y,
        p_position.cx,
        p_position.cy,
        wnd_parent,
        reinterpret_cast<HMENU>(0),
        core_api::get_my_instance(),
        this);
    return m_wnd;
}

void panel_stack_splitter::destroy_window() {
    destroy_children();
    if (m_wnd && IsWindow(m_wnd)) {
        DestroyWindow(m_wnd);
    }
    m_wnd = nullptr;
    m_host.release();
}

void panel_stack_splitter::destroy_children() {
    for (auto& e : m_children) {
        if (e.window.is_valid()) {
            try {
                e.window->destroy_window();
            } catch (...) {
            }
            e.window.release();
        }
        e.child_hwnd = nullptr;
    }
}

void panel_stack_splitter::compute_child_rects(int w, int h, std::vector<RECT>& out_rects) {
    out_rects.assign(m_children.size(), RECT{ 0, 0, 0, 0 });
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    if (!m_script.empty()) {
        eval_expr(m_script, w, h, true);
    }

    // Collect non-forced proportional children (arranged horizontally, left-to-right).
    double total_weight = 0.0;
    std::vector<size_t> prop_indices;
    for (size_t i = 0; i < m_children.size(); ++i) {
        const auto& e = m_children[i];
        if (e.forced_layout != 0) continue;
        // If hidden_on_start before window exists, or explicitly hidden proportional panel, still track visible ones.
        bool is_vis = e.child_hwnd ? ((GetWindowLongPtrW(e.child_hwnd, GWL_STYLE) & WS_VISIBLE) != 0) : (e.hidden_on_start == 0);
        if (!is_vis) continue;
        double wt = e.weight > 0.0 ? e.weight : 100.0;
        total_weight += wt;
        prop_indices.push_back(i);
    }

    if (!prop_indices.empty() && total_weight > 0.0) {
        double accum_x = 0.0;
        for (size_t k = 0; k < prop_indices.size(); ++k) {
            size_t idx = prop_indices[k];
            double wt = m_children[idx].weight > 0.0 ? m_children[idx].weight : 100.0;
            int left = static_cast<int>(std::floor(accum_x + 0.5));
            accum_x += (wt / total_weight) * static_cast<double>(w);
            int right = (k + 1 == prop_indices.size()) ? w : static_cast<int>(std::floor(accum_x + 0.5));
            if (right < left) right = left;
            out_rects[idx] = RECT{ left, 0, right, h };
        }
    }

    for (size_t i = 0; i < m_children.size(); ++i) {
        const auto& e = m_children[i];
        if (e.forced_layout == 0) continue;
        int x = eval_int(e.expr_x, w, h);
        int y = eval_int(e.expr_y, w, h);
        int cw = (std::max)(0, eval_int(e.expr_w, w, h));
        int ch = (std::max)(0, eval_int(e.expr_h, w, h));
        out_rects[i] = RECT{ x, y, x + cw, y + ch };
    }
}

void panel_stack_splitter::fill_solid_bg(HDC hdc, const RECT& rc) {
    if (!hdc) return;
    if (!m_bg_brush || m_bg_brush_color != m_bg_color) {
        if (m_bg_brush) {
            DeleteObject(m_bg_brush);
            m_bg_brush = nullptr;
        }
        m_bg_brush = CreateSolidBrush(m_bg_color);
        m_bg_brush_color = m_bg_color;
    }
    if (m_bg_brush) {
        FillRect(hdc, &rc, m_bg_brush);
    }
}

void panel_stack_splitter::instantiate_child(pss_child_entry& e, int parent_w, int parent_h, HWND prev_z) {
    (void)parent_w;
    (void)parent_h;
    if (!uie::window::create_by_guid(e.panel_guid, e.window) || !e.window.is_valid()) return;

    if (e.cfg_data.get_size() > 0) {
        try {
            abort_callback_dummy ab;
            e.window->set_config_from_ptr(e.cfg_data.get_ptr(), e.cfg_data.get_size(), ab);
        } catch (...) {
        }
    }

    ui_helpers::window_position_t pos(
        e.saved_rect.left,
        e.saved_rect.top,
        (std::max)(0L, e.saved_rect.right - e.saved_rect.left),
        (std::max)(0L, e.saved_rect.bottom - e.saved_rect.top));

    try {
        e.child_hwnd = e.window->create_or_transfer_window(
            m_wnd,
            uie::window_host_ptr(m_child_host.get_ptr()),
            pos);
    } catch (...) {
        e.child_hwnd = nullptr;
    }

    if (e.child_hwnd && IsWindow(e.child_hwnd)) {
        ensure_clipsiblings(e.child_hwnd);
        SetWindowPos(e.child_hwnd, prev_z, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(e.child_hwnd, e.hidden_on_start ? SW_HIDE : SW_SHOWNA);
    }
}

void panel_stack_splitter::create_children() {
    if (!m_wnd || !IsWindow(m_wnd)) return;
    RECT rc{};
    GetClientRect(m_wnd, &rc);
    std::vector<RECT> rects;
    compute_child_rects(rc.right - rc.left, rc.bottom - rc.top, rects);

    HWND prev_z = HWND_TOP;
    for (size_t i = 0; i < m_children.size(); ++i) {
        m_children[i].saved_rect = rects[i];
        instantiate_child(m_children[i], rc.right - rc.left, rc.bottom - rc.top, prev_z);
        if (m_children[i].child_hwnd && IsWindow(m_children[i].child_hwnd)) {
            prev_z = m_children[i].child_hwnd;
        }
    }
    relayout();
}

void panel_stack_splitter::relayout() {
    if (m_in_layout || !m_wnd || !IsWindow(m_wnd)) return;
    m_in_layout = true;

    RECT rc{};
    GetClientRect(m_wnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    std::vector<RECT> rects;
    compute_child_rects(w, h, rects);

    // Maintain exact list order in Z-order: index 0 topmost -> index N-1 bottommost.
    // Do NOT alter WS_VISIBLE here so external .Show() toggles (topbar.js) are preserved.
    HWND prev_z = HWND_TOP;
    for (size_t i = 0; i < m_children.size(); ++i) {
        HWND cw = m_children[i].child_hwnd;
        if (!cw || !IsWindow(cw)) continue;
        ensure_clipsiblings(cw);
        const RECT& r = rects[i];
        int rw = (std::max)(0L, r.right - r.left);
        int rh = (std::max)(0L, r.bottom - r.top);
        SetWindowPos(cw, prev_z, r.left, r.top, rw, rh, SWP_NOACTIVATE);
        m_children[i].saved_rect = r;
        prev_z = cw;
    }

    m_in_layout = false;
}

void panel_stack_splitter::on_child_relinquished(HWND wnd) {
    for (auto& e : m_children) {
        if (e.child_hwnd == wnd) {
            e.child_hwnd = nullptr;
            e.window.release();
        }
    }
}

void panel_stack_splitter::on_child_visibility_changed(HWND wnd) {
    for (const auto& e : m_children) {
        if (e.child_hwnd == wnd && e.forced_layout == 0) {
            relayout();
            break;
        }
    }
}

void panel_stack_splitter::collect_children(pfc::list_base_t<uie::window_ptr>& p_out) const {
    for (const auto& e : m_children) {
        if (e.window.is_valid()) p_out.add_item(e.window);
    }
}

LRESULT CALLBACK panel_stack_splitter::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    panel_stack_splitter* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<panel_stack_splitter*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_wnd = hwnd;
    } else {
        self = reinterpret_cast<panel_stack_splitter*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        LRESULT rv = self->handle_message(msg, wp, lp);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->m_wnd = nullptr;
        }
        return rv;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT panel_stack_splitter::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        create_children();
        return 0;

    case WM_SIZE:
        relayout();
        return 0;

    case WM_WINDOWPOSCHANGED: {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        if (pos && !(pos->flags & SWP_NOSIZE)) {
            relayout();
        }
        return DefWindowProcW(m_wnd, msg, wp, lp);
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        if (mmi) {
            mmi->ptMinTrackSize.x = 0;
            mmi->ptMinTrackSize.y = 0;
            mmi->ptMaxTrackSize.x = MAXLONG;
            mmi->ptMaxTrackSize.y = MAXLONG;
        }
        return 0;
    }

    case WM_ERASEBKGND:
    case WM_PRINTCLIENT: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        if (hdc) {
            RECT rc{};
            GetClientRect(m_wnd, &rc);
            fill_solid_bg(hdc, rc);
            return 1;
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(m_wnd, &ps);
        fill_solid_bg(hdc, ps.rcPaint);
        EndPaint(m_wnd, &ps);
        return 0;
    }

    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_TIMECHANGE:
        for (const auto& e : m_children) {
            if (e.child_hwnd && IsWindow(e.child_hwnd)) {
                SendMessageW(e.child_hwnd, msg, wp, lp);
            }
        }
        return 0;

    case WM_DESTROY:
        destroy_children();
        if (m_bg_brush) {
            DeleteObject(m_bg_brush);
            m_bg_brush = nullptr;
            m_bg_brush_color = CLR_INVALID;
        }
        return 0;

    default:
        return DefWindowProcW(m_wnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------
// uie::splitter_window / splitter_window_v2 editing support
// ---------------------------------------------------------------------------

bool panel_stack_splitter::get_config_item_supported(t_size p_index, const GUID& p_type) const {
    if (p_index >= m_children.size()) return false;
    return p_type == uie::splitter_window::bool_hidden ||
           p_type == uie::splitter_window::bool_use_custom_title ||
           p_type == uie::splitter_window::string_custom_title;
}

bool panel_stack_splitter::get_config_item(
    t_size index, const GUID& p_type, stream_writer* p_out, abort_callback& p_abort) const {
    if (index >= m_children.size() || !p_out) return false;
    const auto& e = m_children[index];
    if (p_type == uie::splitter_window::bool_hidden) {
        bool v = e.hidden_on_start != 0;
        p_out->write(&v, sizeof(v), p_abort);
        return true;
    }
    if (p_type == uie::splitter_window::bool_use_custom_title) {
        bool v = !e.name.empty();
        p_out->write(&v, sizeof(v), p_abort);
        return true;
    }
    if (p_type == uie::splitter_window::string_custom_title) {
        p_out->write_string(e.name.data(), e.name.size(), p_abort);
        return true;
    }
    return false;
}

bool panel_stack_splitter::set_config_item(
    t_size index, const GUID& p_type, stream_reader* p_source, abort_callback& p_abort) {
    if (index >= m_children.size() || !p_source) return false;
    auto& e = m_children[index];
    if (p_type == uie::splitter_window::bool_hidden) {
        bool v = false;
        p_source->read_object(&v, sizeof(v), p_abort);
        e.hidden_on_start = v ? 1 : 0;
        if (e.child_hwnd && IsWindow(e.child_hwnd)) {
            ShowWindow(e.child_hwnd, v ? SW_HIDE : SW_SHOWNA);
            if (e.forced_layout == 0) relayout();
        }
        return true;
    }
    if (p_type == uie::splitter_window::bool_use_custom_title) {
        bool v = false;
        p_source->read_object(&v, sizeof(v), p_abort);
        if (!v) e.name.clear();
        return true;
    }
    if (p_type == uie::splitter_window::string_custom_title) {
        pfc::string8 s;
        p_source->read_string(s, p_abort);
        e.name = s.get_ptr();
        return true;
    }
    return false;
}

void panel_stack_splitter::read_item_into_entry(const uie::splitter_item_t* src, pss_child_entry& dst) {
    dst.panel_guid = src->get_panel_guid();
    dst.cfg_data = src->get_panel_config_to_array(false);
    const uie::splitter_item_full_t* full = nullptr;
    if (src->query(full) && full) {
        pfc::string8 t;
        full->get_title(t);
        if (full->m_custom_title && !t.is_empty()) {
            dst.name = t.get_ptr();
        }
        dst.hidden_on_start = full->m_hidden ? 1 : 0;
        if (full->m_size > 0) dst.weight = static_cast<double>(full->m_size);
    }
    if (dst.name.empty()) {
        uie::window_ptr tmp;
        if (uie::window::create_by_guid(dst.panel_guid, tmp)) {
            pfc::string8 n;
            tmp->get_name(n);
            dst.name = n.get_ptr();
        }
    }
}

void panel_stack_splitter::insert_panel(t_size index, const uie::splitter_item_t* p_item) {
    if (!p_item) return;
    if (index > m_children.size()) index = m_children.size();
    pss_child_entry entry;
    read_item_into_entry(p_item, entry);
    m_children.insert(m_children.begin() + index, std::move(entry));

    if (m_wnd && IsWindow(m_wnd)) {
        RECT rc{};
        GetClientRect(m_wnd, &rc);
        instantiate_child(m_children[index], rc.right - rc.left, rc.bottom - rc.top, HWND_TOP);
        relayout();
    }
}

void panel_stack_splitter::remove_panel(t_size index) {
    if (index >= m_children.size()) return;
    auto& e = m_children[index];
    if (e.window.is_valid()) {
        try {
            e.window->destroy_window();
        } catch (...) {
        }
        e.window.release();
    }
    e.child_hwnd = nullptr;
    m_children.erase(m_children.begin() + index);
    if (m_wnd && IsWindow(m_wnd)) relayout();
}

void panel_stack_splitter::replace_panel(t_size index, const uie::splitter_item_t* p_item) {
    if (!p_item || index >= m_children.size()) return;
    auto& old_e = m_children[index];
    if (old_e.window.is_valid()) {
        try {
            old_e.window->destroy_window();
        } catch (...) {
        }
        old_e.window.release();
    }
    old_e.child_hwnd = nullptr;

    pss_child_entry next_e = old_e;
    next_e.window.release();
    next_e.child_hwnd = nullptr;
    read_item_into_entry(p_item, next_e);
    m_children[index] = std::move(next_e);

    if (m_wnd && IsWindow(m_wnd)) {
        RECT rc{};
        GetClientRect(m_wnd, &rc);
        instantiate_child(m_children[index], rc.right - rc.left, rc.bottom - rc.top, HWND_TOP);
        relayout();
    }
}

uie::splitter_item_t* panel_stack_splitter::get_panel(t_size index) const {
    auto* item = new uie::splitter_item_full_v3_impl_t();
    if (index >= m_children.size()) return item;
    refresh_live_configs();
    const auto& e = m_children[index];
    item->set_panel_guid(e.panel_guid);
    if (e.cfg_data.get_size() > 0) {
        item->set_panel_config_from_ptr(e.cfg_data.get_ptr(), e.cfg_data.get_size());
    }
    item->set_window_ptr(e.window);
    item->m_hidden = (e.hidden_on_start != 0);
    item->m_custom_title = !e.name.empty();
    item->set_title(e.name.c_str(), e.name.size());
    item->m_size = static_cast<uint32_t>((std::max)(1.0, e.weight));
    return item;
}

bool panel_stack_splitter::is_point_ours(
    HWND wnd_point, const POINT& pt_screen, pfc::list_base_t<uie::window_ptr>& p_hierarchy) {
    if (!m_wnd || !IsWindow(m_wnd)) return false;
    if (wnd_point != m_wnd && !IsChild(m_wnd, wnd_point)) return false;
    p_hierarchy.add_item(uie::window_ptr(this));
    for (const auto& e : m_children) {
        if (!e.window.is_valid() || !e.child_hwnd) continue;
        if (wnd_point == e.child_hwnd || IsChild(e.child_hwnd, wnd_point)) {
            uie::splitter_window_v2::ptr sp;
            if (e.window->service_query_t(sp)) {
                sp->is_point_ours(wnd_point, pt_screen, p_hierarchy);
            } else {
                p_hierarchy.add_item(e.window);
            }
            break;
        }
    }
    return true;
}

void panel_stack_splitter::get_supported_panels(
    const pfc::list_base_const_t<uie::window_ptr>& p_windows, bit_array_var& p_mask_unsupported) {
    const t_size n = p_windows.get_count();
    for (t_size i = 0; i < n; ++i) {
        bool supported = p_windows[i].is_valid() &&
            p_windows[i]->is_available(uie::window_host_ptr(m_child_host.get_ptr()));
        p_mask_unsupported.set(i, !supported);
    }
}

static uie::window_factory<panel_stack_splitter> g_pss_window_factory;

} // namespace pss
