#ifndef XASSET_H
#define XASSET_H

#include <iostream>
#include <string>
#include <map>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <cctype>
#include <cstring>   // std::strcmp
#include <sqlite3.h> // kept in case other parts still use it
#include "db.h"

class xasset
{
public:
    std::string Name = "";
    std::string Type = "";
    std::string GdtRelPath = "";
    std::map<std::string, std::string> value;

    xasset() = default;

    xasset(std::string nameInput, std::string typeInput)
        : Name(std::move(nameInput)), Type(std::move(typeInput))
    {
    }

    // ------------- Edit/Reset helpers (unchanged) -------------
    bool EditValue(const std::string &key, const std::string &newValue)
    {
        auto it = value.find(key);
        if (it != value.end())
        {
            it->second = newValue;
            return true;
        }
        std::cerr << "Warning: key '" << key << "' not found in asset '" << Name << "'\n";
        return false;
    }

    std::string GetValue(const std::string& key)
    {
        auto it = value.find(key);
        if (it != value.end())
        {
            return it->second;
        }
        std::cerr << "Warning: key '" << key << "' not found in asset '" << Name << "'\n";
        return "";
    }

    bool ResetValue(const std::string &key)
    {
        auto def = defaultValues.find(key);
        if (def != defaultValues.end())
        {
            value[key] = def->second;
            return true;
        }
        std::cerr << "Warning: default value for key '" << key << "' not found in asset '" << Name << "'\n";
        return false;
    }

    void ResetAllValues() { value = defaultValues; }

    void PrintValues() const
    {
        std::cout << "Asset: " << Name << " (" << Type << ")\n";
        for (const auto &[k, v] : value)
            std::cout << "  " << k << " = \"" << v << "\"\n";
    }

    // ======================================================================
    // NEW: Parse the .gdt FILE to get CURRENT values for this asset
    // ======================================================================

    // Parse values from an ABSOLUTE GDT path. Returns a {key -> value} map.
    std::map<std::string, std::string> GetValues(const std::wstring &gdtAbsPath) const
    {
        std::map<std::string, std::string> out;

        // ---- load file into memory
        std::ifstream in(WideToUTF8(gdtAbsPath), std::ios::in | std::ios::binary);
        if (!in)
        {
            std::cerr << "Error: cannot open GDT: " << WideToUTF8(gdtAbsPath) << "\n";
            return out;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.empty())
            return out;

        // ---- scan for our asset header: "Name" ( "Type.gdf" ) { ... }
        Cursor c{text.data(), text.data() + text.size()};
        std::string_view svName, svGdf;

        while (c.ok())
        {
            skip_ws(c);
            if (!c.ok())
                break;

            const char *save = c.p;
            if (!scan_quoted(c, svName))
            {
                ++c.p;
                continue;
            }

            // must match asset name
            if (!iequals(svName, Name))
            {
                c.p = save + 1;
                continue;
            }

            skip_ws(c);
            if (!scan_char(c, '('))
            {
                c.p = save + 1;
                continue;
            }
            skip_ws(c);
            if (!scan_quoted(c, svGdf))
            {
                c.p = save + 1;
                continue;
            }
            skip_ws(c);
            if (!scan_char(c, ')'))
            {
                c.p = save + 1;
                continue;
            }
            skip_ws(c);

            // optional: ensure gdf stem equals our Type
            std::string_view gdfStem = StemNoExt(svGdf);
            if (!iequals(gdfStem, Type))
            {
                // Not our type; keep searching for the right block
                c.p = save + 1;
                continue;
            }

            if (!scan_char(c, '{'))
            {
                c.p = save + 1;
                continue;
            }

            // We are at the body: parse key/value pairs until matching '}'
            parse_body_into_map(c, out);
            break; // done
        }

        return out;
    }

    // Convenience: build absolute path from toolsRoot + relPath and load.
    std::map<std::string, std::string> GetValues(const std::wstring &toolsRoot, const std::wstring &gdtRelPath) const
    {
        return GetValues(Join2(toolsRoot, gdtRelPath));
    }

    // Load values using the remembered GDT relative path + TA_TOOLS_PATH
    void LoadValuesFromSource()
    {
        if (GdtRelPath.empty())
        {
            std::cerr << "xasset[" << Name << "]: GdtRelPath is empty; cannot load.\n";
            return;
        }
        std::wstring toolsRoot = getToolsRootW();
        if (toolsRoot.empty())
        {
            std::cerr << "xasset[" << Name << "]: TA_TOOLS_PATH not set.\n";
            return;
        }
        std::wstring abs = Join2(toolsRoot, UTF8ToWide(GdtRelPath));
        value = GetValues(abs); // uses the file-based GetValues you added earlier
    }

private:
    std::map<std::string, std::string> defaultValues;

    // ================== Small utilities ==================
    static std::string WideToUTF8(const std::wstring &w)
    {
        if (w.empty())
            return {};
        int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }

    static std::wstring Join2(const std::wstring &a, const std::wstring &b)
    {
        if (a.empty())
            return b;
        if (b.empty())
            return a;
        if (a.back() == L'\\' || a.back() == L'/')
            return a + b;
        return a + L'\\' + b;
    }

    static bool iequals(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            unsigned char ca = (unsigned char)a[i];
            unsigned char cb = (unsigned char)b[i];
            if (std::tolower(ca) != std::tolower(cb))
                return false;
        }
        return true;
    }

    static std::string_view StemNoExt(std::string_view s)
    {
        size_t slash = s.find_last_of("/\\");
        if (slash != std::string::npos)
            s.remove_prefix(slash + 1);
        size_t dot = s.find_last_of('.');
        if (dot != std::string::npos)
            s.remove_suffix(s.size() - dot);
        return s;
    }

    static std::string StripQuotes(const char *str)
    {
        if (!str)
            return "";
        std::string s(str);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
        return s;
    }

    // ================== Lightweight scanner used for .gdt ==================
    struct Cursor
    {
        const char *p;
        const char *e;
        bool ok() const { return p && p < e; }
    };

    static void skip_ws(Cursor &c)
    {
        while (c.ok())
        {
            unsigned char ch = (unsigned char)*c.p;
            if (ch > ' ')
                break;
            ++c.p;
        }
    }

    static bool scan_char(Cursor &c, char ch)
    {
        if (!c.ok() || *c.p != ch)
            return false;
        ++c.p;
        return true;
    }

    static bool scan_quoted(Cursor &c, std::string_view &out)
    {
        if (!scan_char(c, '"'))
            return false;
        const char *beg = c.p;
        while (c.ok() && *c.p != '"')
            ++c.p;
        if (!c.ok())
            return false;
        out = std::string_view(beg, (size_t)(c.p - beg));
        ++c.p; // consume closing quote
        return true;
    }

    static bool scan_ident(Cursor &c, std::string &out)
    {
        if (!c.ok())
            return false;
        const char *beg = c.p;
        unsigned char ch = (unsigned char)*c.p;
        if (!(std::isalpha(ch) || ch == '_' || ch == '$'))
            return false;
        ++c.p;
        while (c.ok())
        {
            ch = (unsigned char)*c.p;
            if (!(std::isalnum(ch) || ch == '_' || ch == '$' || ch == '.'))
                break;
            ++c.p;
        }
        out.assign(beg, c.p);
        return true;
    }

    static void parse_body_into_map(Cursor &c, std::map<std::string, std::string> &out)
    {
        // we enter with '{' already consumed by caller
        int depth = 1;
        while (c.ok() && depth > 0)
        {
            skip_ws(c);
            if (!c.ok())
                break;
            if (*c.p == '}')
            {
                --depth;
                ++c.p;
                break;
            }

            // Parse key: either "quoted" or identifier
            std::string key;
            std::string_view keyQ;
            if (scan_quoted(c, keyQ))
            {
                key.assign(keyQ.begin(), keyQ.end());
            }
            else if (!scan_ident(c, key))
            {
                // Unknown token; try to resync
                ++c.p;
                continue;
            }

            skip_ws(c);
            // Optional '='
            if (scan_char(c, '='))
                skip_ws(c);

            // Parse value: prefer quoted string; else capture until EOL/next brace
            std::string valueStr;
            std::string_view valQ;
            if (scan_quoted(c, valQ))
            {
                valueStr.assign(valQ.begin(), valQ.end());
            }
            else
            {
                // unquoted value (rare) read until newline or brace
                const char *beg = c.p;
                while (c.ok() && *c.p != '\n' && *c.p != '\r' && *c.p != '}')
                    ++c.p;
                // trim trailing spaces
                const char *end = c.p;
                while (end > beg && (unsigned char)*(end - 1) <= ' ')
                    --end;
                valueStr.assign(beg, end);
                // If we stopped on '}', we'll let the next loop iteration handle it.
            }

            out[key] = valueStr;

            // Continue until we hit the closing '}' that balances this body.
            skip_ws(c);
            if (!c.ok())
                break;
            if (*c.p == '}')
            {
                --depth;
                ++c.p;
                break;
            }
        }
    }

    // ======== Legacy schema helpers kept for completeness (not used here) ========
    static int SchemaCB(void *ctx, int argc, char **argv, char **)
    {
        auto *out = static_cast<std::map<std::string, std::string> *>(ctx);
        const char *name_raw = (argc > 1 ? argv[1] : nullptr);
        if (!name_raw)
            return 0;
        std::string name = StripQuotes(name_raw);
        if (name == "PK_id" || (!name.empty() && name[0] == '_'))
            return 0;
        const char *dflt_raw = (argc > 4 && argv[4]) ? argv[4] : "";
        std::string dflt = StripQuotes(dflt_raw);
        (*out)[name] = dflt;
        return 0;
    }

    std::map<std::string, std::string> GetInitialValues(const std::string & /*table_name*/) const
    {
        // Intentionally left as a no-op; kept only for API compatibility.
        return {};
    }

    // --- helpers (can be private static inside xasset) ---
    static std::wstring getToolsRootW()
    {
        wchar_t *val = nullptr;
        size_t len = 0;
        if (_wdupenv_s(&val, &len, L"TA_TOOLS_PATH") != 0 || !val)
            return {};
        std::wstring root(val);
        free(val);
        if (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
            root.pop_back();
        return root;
    }
    static std::wstring UTF8ToWide(const std::string &s)
    {
        if (s.empty())
            return {};
        int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    }
};

#endif
