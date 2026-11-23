#include "pss_expr.h"

#include "../foobar2000/SDK/foobar2000-lite.h"
#include "../foobar2000/SDK/cfg_var.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pss {

namespace {

// Matches original 32-bit foo_uie_panel_splitter.dll.cfg global variable store GUID:
// {241D485A-C7BA-4129-9E0E-F6C13F2A03AA}
constexpr GUID kCfgGlobalsGuid = {
    0x241d485a, 0xc7ba, 0x4129, {0x9e, 0x0e, 0xf6, 0xc1, 0x3f, 0x2a, 0x03, 0xaa}
};

std::mutex g_globals_mutex;
std::unordered_map<std::string, std::string> g_globals;

std::string to_lower_ascii(const std::string& s) {
    std::string out = s;
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string trim_ws(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

long long parse_int64(const std::string& s) {
    std::string t = trim_ws(s);
    if (t.empty()) return 0;
    char* endp = nullptr;
    return std::strtoll(t.c_str(), &endp, 10);
}

bool is_truthy(const std::string& s) {
    return !trim_ws(s).empty() && trim_ws(s) != "0";
}

class cfg_pss_globals : public cfg_var {
public:
    explicit cfg_pss_globals(const GUID& p_guid) : cfg_var(p_guid) {}

    void get_data_raw(stream_writer* p_stream, abort_callback& p_abort) override {
        std::lock_guard<std::mutex> lock(g_globals_mutex);
        uint32_t count = static_cast<uint32_t>(g_globals.size());
        p_stream->write_lendian_t(count, p_abort);
        for (const auto& kv : g_globals) {
            uint32_t klen = static_cast<uint32_t>(kv.first.size());
            uint32_t vlen = static_cast<uint32_t>(kv.second.size());
            uint32_t rec_len = 8 + klen + vlen;
            p_stream->write_lendian_t(rec_len, p_abort);
            p_stream->write_lendian_t(klen, p_abort);
            if (klen > 0) p_stream->write(kv.first.data(), klen, p_abort);
            p_stream->write_lendian_t(vlen, p_abort);
            if (vlen > 0) p_stream->write(kv.second.data(), vlen, p_abort);
        }
    }

    void set_data_raw(stream_reader* p_stream, t_size p_sizehint, abort_callback& p_abort) override {
        if (p_sizehint < 4) return;
        uint32_t count = 0;
        p_stream->read_lendian_t(count, p_abort);
        if (count > 4096) return;

        std::unordered_map<std::string, std::string> loaded;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t rec_len = 0, klen = 0, vlen = 0;
            p_stream->read_lendian_t(rec_len, p_abort);
            p_stream->read_lendian_t(klen, p_abort);
            if (klen > 65536) return;
            std::string key(klen, '\0');
            if (klen > 0) p_stream->read(&key[0], klen, p_abort);
            p_stream->read_lendian_t(vlen, p_abort);
            if (vlen > 65536) return;
            std::string val(vlen, '\0');
            if (vlen > 0) p_stream->read(&val[0], vlen, p_abort);
            loaded[key] = val;
        }
        std::lock_guard<std::mutex> lock(g_globals_mutex);
        g_globals.swap(loaded);
    }
};

cfg_pss_globals g_cfg_globals(kCfgGlobalsGuid);

struct eval_ctx {
    const std::string& src;
    size_t pos = 0;
    int ps_width = 0;
    int ps_height = 0;
    bool allow_side_effects = false;

    std::string eval_until(bool stop_on_arg_delim) {
        std::string out;
        while (pos < src.size()) {
            char c = src[pos];
            if (stop_on_arg_delim && (c == ',' || c == ')')) {
                break;
            }
            if (c == '\'') {
                ++pos;
                if (pos < src.size() && src[pos] == '\'') {
                    out.push_back('\'');
                    ++pos;
                } else {
                    while (pos < src.size()) {
                        if (src[pos] == '\'') {
                            ++pos;
                            break;
                        }
                        out.push_back(src[pos++]);
                    }
                }
                continue;
            }
            if (c == '%') {
                size_t endp = src.find('%', pos + 1);
                if (endp != std::string::npos) {
                    std::string var = src.substr(pos + 1, endp - pos - 1);
                    pos = endp + 1;
                    out += resolve_var(var);
                    continue;
                }
            }
            if (c == '$') {
                std::string call_res;
                if (try_parse_call(call_res)) {
                    out += call_res;
                    continue;
                }
            }
            out.push_back(c);
            ++pos;
        }
        return out;
    }

    std::string resolve_var(const std::string& name) const {
        if (name.empty()) return "%";
        std::string low = to_lower_ascii(name);
        if (low == "ps_width") return std::to_string(ps_width);
        if (low == "ps_height") return std::to_string(ps_height);
        return get_global(name);
    }

    bool try_parse_call(std::string& result) {
        size_t p = pos + 1;
        while (p < src.size() && (std::isalnum(static_cast<unsigned char>(src[p])) || src[p] == '_')) {
            ++p;
        }
        if (p == pos + 1 || p >= src.size() || src[p] != '(') {
            return false;
        }
        std::string func = to_lower_ascii(src.substr(pos + 1, p - pos - 1));
        pos = p + 1;

        std::vector<std::string> args;
        // For lazy conditionals and $set_ps_global key preservation, capture raw spans first when needed,
        // or parse lazily only if conditional: keep simple by tracking nesting for $if/$ifgreater/$ifequal.
        if (func == "if" || func == "if2" || func == "ifgreater" || func == "ifequal" || func == "iflonger") {
            std::vector<std::string> raw_args = split_raw_args();
            result = eval_conditional(func, raw_args);
            return true;
        }

        while (pos < src.size()) {
            args.push_back(eval_until(true));
            if (pos >= src.size()) break;
            if (src[pos] == ')') {
                ++pos;
                break;
            }
            if (src[pos] == ',') {
                ++pos;
            }
        }
        result = invoke_func(func, args);
        return true;
    }

    std::vector<std::string> split_raw_args() {
        std::vector<std::string> raw;
        size_t start = pos;
        int depth = 1;
        bool in_quote = false;
        while (pos < src.size() && depth > 0) {
            char c = src[pos];
            if (c == '\'') {
                in_quote = !in_quote;
                ++pos;
                continue;
            }
            if (!in_quote) {
                if (c == '(') {
                    ++depth;
                } else if (c == ')') {
                    --depth;
                    if (depth == 0) {
                        raw.push_back(src.substr(start, pos - start));
                        ++pos;
                        return raw;
                    }
                } else if (c == ',' && depth == 1) {
                    raw.push_back(src.substr(start, pos - start));
                    ++pos;
                    start = pos;
                    continue;
                }
            }
            ++pos;
        }
        if (start <= src.size()) {
            raw.push_back(src.substr(start));
        }
        return raw;
    }

    std::string subeval(const std::string& s) const {
        eval_ctx sub{s, 0, ps_width, ps_height, allow_side_effects};
        return sub.eval_until(false);
    }

    std::string eval_conditional(const std::string& func, const std::vector<std::string>& raw) const {
        auto get_raw = [&](size_t i) -> std::string { return i < raw.size() ? subeval(raw[i]) : std::string(); };
        if (func == "if") {
            if (raw.empty()) return "";
            std::string cond = get_raw(0);
            return is_truthy(cond) ? get_raw(1) : get_raw(2);
        }
        if (func == "if2") {
            std::string a = get_raw(0);
            return is_truthy(a) ? a : get_raw(1);
        }
        if (func == "ifgreater") {
            return parse_int64(get_raw(0)) > parse_int64(get_raw(1)) ? get_raw(2) : get_raw(3);
        }
        if (func == "ifequal") {
            return parse_int64(get_raw(0)) == parse_int64(get_raw(1)) ? get_raw(2) : get_raw(3);
        }
        if (func == "iflonger") {
            return static_cast<long long>(get_raw(0).size()) > parse_int64(get_raw(1)) ? get_raw(2) : get_raw(3);
        }
        return "";
    }

    std::string invoke_func(const std::string& func, const std::vector<std::string>& args) const {
        auto arg_i = [&](size_t idx) -> long long {
            return idx < args.size() ? parse_int64(args[idx]) : 0;
        };
        auto arg_s = [&](size_t idx) -> const std::string& {
            static const std::string empty;
            return idx < args.size() ? args[idx] : empty;
        };

        if (func == "set_ps_global") {
            if (allow_side_effects && args.size() >= 2) {
                set_global(trim_ws(args[0]), args[1]);
            }
            return "";
        }
        if (func == "add") {
            long long sum = 0;
            for (size_t i = 0; i < args.size(); ++i) sum += parse_int64(args[i]);
            return std::to_string(sum);
        }
        if (func == "sub") {
            if (args.empty()) return "0";
            long long res = parse_int64(args[0]);
            for (size_t i = 1; i < args.size(); ++i) res -= parse_int64(args[i]);
            return std::to_string(res);
        }
        if (func == "mul") {
            if (args.empty()) return "0";
            long long res = 1;
            for (size_t i = 0; i < args.size(); ++i) res *= parse_int64(args[i]);
            return std::to_string(res);
        }
        if (func == "div") {
            if (args.empty()) return "0";
            long long res = parse_int64(args[0]);
            for (size_t i = 1; i < args.size(); ++i) {
                long long d = parse_int64(args[i]);
                if (d == 0) return "0";
                res /= d;
            }
            return std::to_string(res);
        }
        if (func == "mod") {
            long long d = arg_i(1);
            return d == 0 ? "0" : std::to_string(arg_i(0) % d);
        }
        if (func == "min") {
            if (args.empty()) return "0";
            long long m = parse_int64(args[0]);
            for (size_t i = 1; i < args.size(); ++i) m = (std::min)(m, parse_int64(args[i]));
            return std::to_string(m);
        }
        if (func == "max") {
            if (args.empty()) return "0";
            long long m = parse_int64(args[0]);
            for (size_t i = 1; i < args.size(); ++i) m = (std::max)(m, parse_int64(args[i]));
            return std::to_string(m);
        }
        if (func == "greater") return arg_i(0) > arg_i(1) ? "1" : "";
        if (func == "strcmp") return arg_s(0) == arg_s(1) ? "1" : "";
        if (func == "stricmp") return to_lower_ascii(arg_s(0)) == to_lower_ascii(arg_s(1)) ? "1" : "";
        if (func == "not") return is_truthy(arg_s(0)) ? "" : "1";
        if (func == "and") {
            for (const auto& a : args) {
                if (!is_truthy(a)) return "";
            }
            return args.empty() ? "" : "1";
        }
        if (func == "or") {
            for (const auto& a : args) {
                if (is_truthy(a)) return "1";
            }
            return "";
        }
        if (func == "trim") return trim_ws(arg_s(0));
        if (func == "len") return std::to_string(arg_s(0).size());
        return "";
    }
};

} // namespace

std::string get_global(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_globals_mutex);
    auto it = g_globals.find(name);
    if (it != g_globals.end()) return it->second;
    std::string target_low = to_lower_ascii(name);
    for (const auto& kv : g_globals) {
        if (to_lower_ascii(kv.first) == target_low) return kv.second;
    }
    return "";
}

void set_global(const std::string& name, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_globals_mutex);
    g_globals[name] = value;
}

std::string eval_expr(const std::string& expr, int ps_width, int ps_height, bool allow_side_effects) {
    eval_ctx ctx{expr, 0, ps_width, ps_height, allow_side_effects};
    return ctx.eval_until(false);
}

int eval_int(const std::string& expr, int ps_width, int ps_height) {
    return static_cast<int>(parse_int64(eval_expr(expr, ps_width, ps_height, false)));
}

} // namespace pss
