#include "channel_spectrum.h"

#include <commdlg.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace cs {

namespace {

enum CtrlId : WORD {
    IDC_BTN_FG1 = 101,
    IDC_BTN_FG2 = 102,
    IDC_BTN_BG = 103,
    IDC_BTN_PEAK = 104,
    IDC_CB_COLOR_MODE = 110,
    IDC_CB_BORDER = 111,
    IDC_CB_CHANNEL_MODE = 112,
    IDC_CB_FFT_SIZE = 113,
    IDC_CB_FFT_MODE = 114,
    IDC_CB_ROTATION = 115,
    IDC_CB_CHANNEL_SINGLE = 116,
    IDC_CB_CHANNEL_DIR = 117,
    IDC_ED_BAR_WIDTH = 120,
    IDC_ED_SPACING = 121,
    IDC_ED_REFRESH = 122,
    IDC_ED_PEAK_HOLD = 123,
    IDC_ED_PEAK_FALL = 124,
    IDC_ED_SCALE_PCT = 125,
    IDC_ED_BLUR = 126,
    IDC_ED_CHAN_SPACING = 127,
    IDC_CHK_SHOW_PEAK = 130,
    IDC_CHK_GRID_MODE = 131,
    IDC_CHK_LOG_AMP = 132,
    IDC_CHK_LIN_FREQ = 133,
    IDC_CHK_CUSTOM_FG = 134,
    IDC_CHK_FLIP_FREQ = 135,
    IDC_CHK_FLIP_CHAN = 136,
    IDC_CHK_TRANSPARENT = 137,
    IDC_CHK_FAST_PSEUDO = 138,
    IDC_CHK_SYNC_SCANLINE = 139,
    IDC_BTN_APPLY = 140,
    IDC_BTN_RESET = 141,
};

struct dlg_state {
    channel_spectrum_window* owner = nullptr;
    cs_config cfg;
    COLORREF custom_palette[16]{};
};

// Simple dynamic DLGTEMPLATE builder to avoid .rc resource compiler coupling.
class dlg_builder {
public:
    void begin(const wchar_t* title, short w, short h) {
        m_buf.clear();
        align4();
        push_u32(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT);
        push_u32(0); // exStyle
        push_u16(0); // count placeholder (offset 4 in u16s -> byte 8)
        push_u16(0);
        push_u16(0);
        push_u16(static_cast<uint16_t>(w));
        push_u16(static_cast<uint16_t>(h));
        push_u16(0); // menu
        push_u16(0); // class
        push_wstr(title);
        push_u16(9); // point size
        push_wstr(L"Microsoft YaHei UI");
    }

    void add(WORD cls_atom, WORD id, DWORD style, short x, short y, short w, short h, const wchar_t* text) {
        align4();
        push_u32(style | WS_CHILD | WS_VISIBLE);
        push_u32(0);
        push_u16(static_cast<uint16_t>(x));
        push_u16(static_cast<uint16_t>(y));
        push_u16(static_cast<uint16_t>(w));
        push_u16(static_cast<uint16_t>(h));
        push_u16(id);
        push_u16(0xFFFF);
        push_u16(cls_atom);
        push_wstr(text);
        push_u16(0); // extra
        ++m_count;
    }

    const DLGTEMPLATE* finalize() {
        uint16_t* p = reinterpret_cast<uint16_t*>(m_buf.data());
        p[4] = m_count;
        return reinterpret_cast<const DLGTEMPLATE*>(m_buf.data());
    }

private:
    void align4() {
        while (m_buf.size() % 4 != 0) m_buf.push_back(0);
    }
    void push_u16(uint16_t v) {
        m_buf.push_back(static_cast<uint8_t>(v & 0xFF));
        m_buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }
    void push_u32(uint32_t v) {
        push_u16(static_cast<uint16_t>(v & 0xFFFF));
        push_u16(static_cast<uint16_t>((v >> 16) & 0xFFFF));
    }
    void push_wstr(const wchar_t* s) {
        while (*s) {
            push_u16(static_cast<uint16_t>(*s++));
        }
        push_u16(0);
    }

    std::vector<uint8_t> m_buf;
    uint16_t m_count = 0;
};

void set_combo_items(HWND dlg, WORD id, const wchar_t* const* items, int count, int sel) {
    HWND cb = GetDlgItem(dlg, id);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < count; ++i) {
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i]));
    }
    SendMessageW(cb, CB_SETCURSEL, std::clamp(sel, 0, count - 1), 0);
}

int fft_to_index(uint32_t fft) {
    switch (fft) {
    case 512: return 0;
    case 1024: return 1;
    case 2048: return 2;
    case 4096: return 3;
    case 8192: return 4;
    default: return 2;
    }
}

uint32_t index_to_fft(int idx) {
    const uint32_t table[5] = { 512, 1024, 2048, 4096, 8192 };
    return (idx >= 0 && idx < 5) ? table[idx] : 2048;
}

void update_enable_states(HWND dlg) {
    const BOOL trans = (IsDlgButtonChecked(dlg, IDC_CHK_TRANSPARENT) == BST_CHECKED);
    EnableWindow(GetDlgItem(dlg, IDC_CHK_FAST_PSEUDO), trans);

    int color_mode = static_cast<int>(SendDlgItemMessageW(dlg, IDC_CB_COLOR_MODE, CB_GETCURSEL, 0, 0));
    EnableWindow(GetDlgItem(dlg, IDC_ED_BLUR), (color_mode == 3 || color_mode == 4));

    int chan_mode = static_cast<int>(SendDlgItemMessageW(dlg, IDC_CB_CHANNEL_MODE, CB_GETCURSEL, 0, 0));
    EnableWindow(GetDlgItem(dlg, IDC_CB_CHANNEL_SINGLE), (chan_mode == 2));
}

void load_controls(HWND dlg, const cs_config& cfg) {
    const wchar_t* color_modes[] = { L"简单", L"水平", L"垂直", L"简单-反锯齿", L"水平-反锯齿", L"曲线" };
    const wchar_t* borders[] = { L"无", L"凹陷", L"灰色边框" };
    const wchar_t* chans[] = { L"分散", L"集中 (联合)", L"按下面指定" };
    const wchar_t* single_chans[] = { L"左前", L"右前", L"中置", L"低音", L"左后", L"右后" };
    const wchar_t* chan_dirs[] = { L"反射", L"右", L"左" };
    const wchar_t* ffts[] = { L"512", L"1024", L"2048", L"4096", L"8192" };
    const wchar_t* fft_modes[] = { L"旧方法", L"新方法(0.9.5.2+)" };
    const wchar_t* rots[] = { L"0", L"90", L"180", L"270" };

    set_combo_items(dlg, IDC_CB_COLOR_MODE, color_modes, 6, static_cast<int>(cfg.color_mode));
    set_combo_items(dlg, IDC_CB_BORDER, borders, 3, static_cast<int>(cfg.border_style));
    set_combo_items(dlg, IDC_CB_CHANNEL_MODE, chans, 3, static_cast<int>(cfg.channel_mode));
    set_combo_items(dlg, IDC_CB_CHANNEL_SINGLE, single_chans, 6, static_cast<int>(cfg.channel_single % 6u));
    set_combo_items(dlg, IDC_CB_CHANNEL_DIR, chan_dirs, 3, static_cast<int>(cfg.channel_dir % 3u));
    set_combo_items(dlg, IDC_CB_FFT_SIZE, ffts, 5, fft_to_index(cfg.fft_size));
    set_combo_items(dlg, IDC_CB_FFT_MODE, fft_modes, 2, cfg.fft_new ? 1 : 0);
    set_combo_items(dlg, IDC_CB_ROTATION, rots, 4, static_cast<int>(cfg.rotation));

    SetDlgItemInt(dlg, IDC_ED_BAR_WIDTH, cfg.bar_width, FALSE);
    SetDlgItemInt(dlg, IDC_ED_SPACING, cfg.spacing, FALSE);
    SetDlgItemInt(dlg, IDC_ED_REFRESH, cfg.refresh_ms, FALSE);
    SetDlgItemInt(dlg, IDC_ED_SCALE_PCT, cfg.scale_pct, FALSE);
    SetDlgItemInt(dlg, IDC_ED_BLUR, cfg.blur_level, FALSE);
    SetDlgItemInt(dlg, IDC_ED_CHAN_SPACING, cfg.channel_spacing, FALSE);
    SetDlgItemInt(dlg, IDC_ED_PEAK_HOLD, cfg.peak_hold_ms / 10u, FALSE);
    SetDlgItemInt(dlg, IDC_ED_PEAK_FALL, cfg.peak_fall_speed, FALSE);

    CheckDlgButton(dlg, IDC_CHK_SHOW_PEAK, cfg.show_peak ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_GRID_MODE, cfg.grid_mode ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_LOG_AMP, cfg.log_amp ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_LIN_FREQ, cfg.linear_freq ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_CUSTOM_FG, cfg.use_custom_fg ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_FLIP_FREQ, cfg.freq_flip ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_FLIP_CHAN, cfg.chan_flip ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_TRANSPARENT, cfg.transparent ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_FAST_PSEUDO, cfg.fast_pseudo_transparent ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_SYNC_SCANLINE, cfg.sync_scanline ? BST_CHECKED : BST_UNCHECKED);
    update_enable_states(dlg);
}

void save_controls(HWND dlg, cs_config& cfg) {
    auto sel = [dlg](WORD id) {
        int r = static_cast<int>(SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0));
        return (r < 0) ? 0 : r;
    };
    cfg.color_mode = static_cast<uint32_t>(sel(IDC_CB_COLOR_MODE));
    cfg.border_style = static_cast<uint32_t>(sel(IDC_CB_BORDER));
    cfg.channel_mode = static_cast<uint32_t>(sel(IDC_CB_CHANNEL_MODE));
    cfg.channel_single = static_cast<uint32_t>(sel(IDC_CB_CHANNEL_SINGLE));
    cfg.channel_dir = static_cast<uint32_t>(sel(IDC_CB_CHANNEL_DIR));
    cfg.fft_size = index_to_fft(sel(IDC_CB_FFT_SIZE));
    cfg.fft_new = sel(IDC_CB_FFT_MODE) != 0;
    cfg.rotation = static_cast<uint32_t>(sel(IDC_CB_ROTATION));

    BOOL ok = FALSE;
    UINT bw = GetDlgItemInt(dlg, IDC_ED_BAR_WIDTH, &ok, FALSE);
    if (ok) cfg.bar_width = std::clamp<uint32_t>(bw, 1, 256);
    UINT sp = GetDlgItemInt(dlg, IDC_ED_SPACING, &ok, FALSE);
    if (ok) cfg.spacing = std::clamp<uint32_t>(sp, 0, 64);
    UINT rf = GetDlgItemInt(dlg, IDC_ED_REFRESH, &ok, FALSE);
    if (ok) cfg.refresh_ms = std::clamp<uint32_t>(rf, 10, 500);
    UINT sc = GetDlgItemInt(dlg, IDC_ED_SCALE_PCT, &ok, FALSE);
    if (ok) cfg.scale_pct = std::clamp<uint32_t>(sc, 1, 1000);
    UINT bl = GetDlgItemInt(dlg, IDC_ED_BLUR, &ok, FALSE);
    if (ok) cfg.blur_level = std::clamp<uint32_t>(bl, 1, 100);
    UINT csp = GetDlgItemInt(dlg, IDC_ED_CHAN_SPACING, &ok, FALSE);
    if (ok) cfg.channel_spacing = std::clamp<uint32_t>(csp, 0, 99);
    UINT ph = GetDlgItemInt(dlg, IDC_ED_PEAK_HOLD, &ok, FALSE);
    if (ok) cfg.peak_hold_ms = std::clamp<uint32_t>(ph, 0, 1000) * 10u;
    UINT pf = GetDlgItemInt(dlg, IDC_ED_PEAK_FALL, &ok, FALSE);
    if (ok) cfg.peak_fall_speed = std::clamp<uint32_t>(pf, 1, 99);

    cfg.show_peak = IsDlgButtonChecked(dlg, IDC_CHK_SHOW_PEAK) == BST_CHECKED;
    cfg.grid_mode = IsDlgButtonChecked(dlg, IDC_CHK_GRID_MODE) == BST_CHECKED;
    cfg.log_amp = IsDlgButtonChecked(dlg, IDC_CHK_LOG_AMP) == BST_CHECKED;
    cfg.linear_freq = IsDlgButtonChecked(dlg, IDC_CHK_LIN_FREQ) == BST_CHECKED;
    cfg.use_custom_fg = IsDlgButtonChecked(dlg, IDC_CHK_CUSTOM_FG) == BST_CHECKED;
    cfg.freq_flip = IsDlgButtonChecked(dlg, IDC_CHK_FLIP_FREQ) == BST_CHECKED;
    cfg.chan_flip = IsDlgButtonChecked(dlg, IDC_CHK_FLIP_CHAN) == BST_CHECKED;
    cfg.transparent = IsDlgButtonChecked(dlg, IDC_CHK_TRANSPARENT) == BST_CHECKED;
    cfg.fast_pseudo_transparent = IsDlgButtonChecked(dlg, IDC_CHK_FAST_PSEUDO) == BST_CHECKED;
    cfg.sync_scanline = IsDlgButtonChecked(dlg, IDC_CHK_SYNC_SCANLINE) == BST_CHECKED;
    cfg.pack_to_raw();
}

bool pick_color(HWND dlg, dlg_state* st, COLORREF& color) {
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = dlg;
    cc.rgbResult = color;
    cc.lpCustColors = st->custom_palette;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) {
        color = cc.rgbResult;
        return true;
    }
    return false;
}

INT_PTR CALLBACK config_dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<dlg_state*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG:
        st = reinterpret_cast<dlg_state*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
        load_controls(dlg, st->cfg);
        return TRUE;

    case WM_COMMAND: {
        WORD id = LOWORD(wp);
        if (!st) break;
        switch (id) {
        case IDC_BTN_FG1:
            pick_color(dlg, st, st->cfg.fg1);
            return TRUE;
        case IDC_BTN_FG2:
            pick_color(dlg, st, st->cfg.fg2);
            return TRUE;
        case IDC_BTN_BG:
            pick_color(dlg, st, st->cfg.bg);
            return TRUE;
        case IDC_BTN_PEAK:
            pick_color(dlg, st, st->cfg.peak_color);
            return TRUE;
        case IDC_CHK_TRANSPARENT:
            update_enable_states(dlg);
            return TRUE;
        case IDC_CB_COLOR_MODE:
        case IDC_CB_CHANNEL_MODE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                update_enable_states(dlg);
            }
            return TRUE;
        case IDC_BTN_RESET:
            st->cfg.reset_defaults();
            load_controls(dlg, st->cfg);
            return TRUE;
        case IDC_BTN_APPLY:
            save_controls(dlg, st->cfg);
            if (st->owner) st->owner->apply_config(st->cfg);
            return TRUE;
        case IDOK:
            save_controls(dlg, st->cfg);
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

} // namespace

bool show_channel_spectrum_config_dialog(HWND parent, channel_spectrum_window* owner, cs_config& cfg) {
    dlg_builder b;
    b.begin(L"配置", 264, 290);

    constexpr WORD kBtn = 0x0080;
    constexpr WORD kEdit = 0x0081;
    constexpr WORD kStatic = 0x0082;
    constexpr WORD kCombo = 0x0085;

    // Color buttons (left)
    b.add(kBtn, IDC_BTN_FG1, 0, 12, 10, 62, 14, L"前景1...");
    b.add(kBtn, IDC_BTN_FG2, 0, 12, 27, 62, 14, L"前景2...");
    b.add(kBtn, IDC_BTN_BG, 0, 12, 44, 62, 14, L"背景...");
    b.add(kBtn, IDC_BTN_PEAK, 0, 12, 61, 62, 14, L"波峰...");

    // Combos (right)
    b.add(kStatic, 0xFFFF, 0, 98, 12, 48, 10, L"栅格宽度:");
    b.add(kEdit, IDC_ED_BAR_WIDTH, WS_BORDER | WS_TABSTOP | ES_NUMBER, 154, 10, 96, 13, L"");

    b.add(kStatic, 0xFFFF, 0, 98, 29, 48, 10, L"边框样式:");
    b.add(kCombo, IDC_CB_BORDER, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 154, 27, 96, 80, L"");

    b.add(kStatic, 0xFFFF, 0, 98, 46, 48, 10, L"颜色模式:");
    b.add(kCombo, IDC_CB_COLOR_MODE, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 154, 44, 96, 100, L"");

    b.add(kStatic, 0xFFFF, 0, 98, 63, 48, 10, L"声道模式:");
    b.add(kCombo, IDC_CB_CHANNEL_MODE, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 154, 61, 96, 80, L"");

    b.add(kCombo, IDC_CB_CHANNEL_SINGLE, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 154, 78, 96, 90, L"");

    b.add(kStatic, 0xFFFF, 0, 98, 97, 48, 10, L"声道方向:");
    b.add(kCombo, IDC_CB_CHANNEL_DIR, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 154, 95, 50, 70, L"");
    b.add(kStatic, 0xFFFF, 0, 208, 97, 18, 10, L"间距:");
    b.add(kEdit, IDC_ED_CHAN_SPACING, WS_BORDER | WS_TABSTOP | ES_NUMBER, 226, 95, 24, 13, L"");

    b.add(kStatic, 0xFFFF, 0, 98, 114, 48, 10, L"FFT 大小:");
    b.add(kCombo, IDC_CB_FFT_SIZE, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 154, 112, 96, 90, L"");

    b.add(kStatic, 0xFFFF, 0, 98, 131, 48, 10, L"FFT 模式:");
    b.add(kCombo, IDC_CB_FFT_MODE, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 154, 129, 96, 60, L"");

    b.add(kStatic, 0xFFFF, 0, 98, 148, 48, 10, L"旋转度数:");
    b.add(kCombo, IDC_CB_ROTATION, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 154, 146, 96, 80, L"");

    b.add(kStatic, 0xFFFF, 0, 98, 165, 48, 10, L"模糊级别:");
    b.add(kEdit, IDC_ED_BLUR, WS_BORDER | WS_TABSTOP | ES_NUMBER, 154, 163, 42, 13, L"");

    // Numeric inputs (left lower)
    b.add(kStatic, 0xFFFF, 0, 12, 82, 48, 10, L"刷新间隔:");
    b.add(kEdit, IDC_ED_REFRESH, WS_BORDER | WS_TABSTOP | ES_NUMBER, 62, 80, 28, 13, L"");

    b.add(kStatic, 0xFFFF, 0, 12, 99, 48, 10, L"排列比例:");
    b.add(kEdit, IDC_ED_SCALE_PCT, WS_BORDER | WS_TABSTOP | ES_NUMBER, 62, 97, 28, 13, L"");

    b.add(kStatic, 0xFFFF, 0, 12, 116, 48, 10, L"间隔:");
    b.add(kEdit, IDC_ED_SPACING, WS_BORDER | WS_TABSTOP | ES_NUMBER, 62, 114, 28, 13, L"");

    b.add(kStatic, 0xFFFF, 0, 12, 133, 48, 10, L"停留[10ms]:");
    b.add(kEdit, IDC_ED_PEAK_HOLD, WS_BORDER | WS_TABSTOP | ES_NUMBER, 62, 131, 28, 13, L"");

    b.add(kStatic, 0xFFFF, 0, 12, 150, 48, 10, L"降落速度:");
    b.add(kEdit, IDC_ED_PEAK_FALL, WS_BORDER | WS_TABSTOP | ES_NUMBER, 62, 148, 28, 13, L"");

    // Checkboxes
    b.add(kBtn, IDC_CHK_SHOW_PEAK, BS_AUTOCHECKBOX | WS_TABSTOP, 12, 186, 70, 11, L"显示波峰");
    b.add(kBtn, IDC_CHK_GRID_MODE, BS_AUTOCHECKBOX | WS_TABSTOP, 98, 186, 70, 11, L"栅格模式");
    b.add(kBtn, IDC_CHK_CUSTOM_FG, BS_AUTOCHECKBOX | WS_TABSTOP, 178, 186, 74, 11, L"8色前景渐变");

    b.add(kBtn, IDC_CHK_LOG_AMP, BS_AUTOCHECKBOX | WS_TABSTOP, 12, 202, 70, 11, L"对数比例");
    b.add(kBtn, IDC_CHK_LIN_FREQ, BS_AUTOCHECKBOX | WS_TABSTOP, 98, 202, 74, 11, L"线性比例(频率)");
    b.add(kBtn, IDC_CHK_FLIP_FREQ, BS_AUTOCHECKBOX | WS_TABSTOP, 178, 202, 76, 11, L"颠倒频率坐标轴");

    b.add(kBtn, IDC_CHK_TRANSPARENT, BS_AUTOCHECKBOX | WS_TABSTOP, 12, 218, 70, 11, L"透明背景");
    b.add(kBtn, IDC_CHK_FLIP_CHAN, BS_AUTOCHECKBOX | WS_TABSTOP, 98, 218, 74, 11, L"颠倒声道位置");
    b.add(kBtn, IDC_CHK_SYNC_SCANLINE, BS_AUTOCHECKBOX | WS_TABSTOP, 178, 218, 74, 11, L"同步扫描线");

    b.add(kBtn, IDC_CHK_FAST_PSEUDO, BS_AUTOCHECKBOX | WS_TABSTOP, 22, 234, 86, 11, L"快速仿透明模式");

    // Bottom buttons
    b.add(kBtn, IDC_BTN_RESET, WS_TABSTOP, 12, 260, 42, 15, L"重置");
    b.add(kBtn, IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 110, 260, 44, 15, L"确定");
    b.add(kBtn, IDC_BTN_APPLY, WS_TABSTOP, 158, 260, 44, 15, L"应用");
    b.add(kBtn, IDCANCEL, WS_TABSTOP, 206, 260, 44, 15, L"取消");

    dlg_state st;
    st.owner = owner;
    st.cfg = cfg;
    st.cfg.pack_to_raw();

    INT_PTR res = DialogBoxIndirectParamW(
        core_api::get_my_instance(), b.finalize(), parent,
        config_dlg_proc, reinterpret_cast<LPARAM>(&st));
    if (res == IDOK) {
        cfg = st.cfg;
        return true;
    }
    return false;
}

} // namespace cs
