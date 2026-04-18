/*
 * NexMatch_Public.cpp  —  OPEN SOURCE (GitHub)
 * UI, EULA, tray management, log visualization.
 * Sensitive logic lives in NexMatch_Core.lib / .dll.
 *
 * Build:
 *   g++ NexMatch_Public.cpp NexMatch_Core.lib -o NexMatch.exe
 *       -lgdi32 -lgdiplus -lpsapi -lshell32 -lcomctl32 -ldwmapi
 *       -lole32 -lwinhttp -mwindows -std=c++17 -O2 -s
 *       -Wno-deprecated-declarations
 */

#include "NexMatch_Core.h"

#include <windowsx.h>
#include <ole2.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <fstream>
#include <mutex>

using namespace Gdiplus;

// ── Color palette ──────────────────────────────────────────────────────────────
#define C_BG        RGB(10,  12,  18)
#define C_PANEL     RGB(16,  19,  28)
#define C_PANEL2    RGB(20,  24,  36)
#define C_BORDER    RGB(36,  42,  60)
#define C_BORDER2   RGB(52,  62,  88)
#define C_WHITE     RGB(240, 245, 255)
#define C_GREEN     RGB(0,   210, 110)
#define C_GREEN_DIM RGB(0,   140,  72)
#define C_GRAY      RGB(96,  112, 144)
#define C_TEXT      RGB(182, 194, 220)
#define C_TEXT_DIM  RGB(96,  112, 144)

// ── Control / timer IDs ────────────────────────────────────────────────────────
#define IDI_ICON1           101
#define ID_LOG_LIST         1001
#define ID_TIMER_CHECK      2001
#define ID_TIMER_PULSE      2003
#define ID_TIMER_UPLOAD     2005
#define ID_TIMER_TRAY       2006
#define ID_EULA_TEXT        3001
#define ID_BTN_ACCEPT       3002
#define ID_BTN_DECLINE      3003
#define WM_TRAYICON         (WM_USER + 100)
#define ID_TRAY_ICON        9001
#define ID_TRAY_SHOW        9002
#define ID_TRAY_EXIT        9003
#define EULA_W              580
#define EULA_H              520

// ── UI globals ─────────────────────────────────────────────────────────────────
HWND            g_hwnd      = NULL;
HWND            g_hLogList  = NULL;
ULONG_PTR       g_gdipToken = 0;
HINSTANCE       g_hInst     = NULL;
NOTIFYICONDATAW g_nid       = {};
bool            g_trayAdded = false;
bool            g_realClose = false;
static int      g_dotAnim   = 0;
static std::mutex g_logMutex;

// EULA state
static HWND g_hEulaWnd         = NULL;
static bool g_eulaAccepted      = false;
static bool g_eulaFinished      = false;
static bool g_eulaHoverAccept   = false;
static bool g_eulaHoverDecline  = false;

// ── Crash logging ──────────────────────────────────────────────────────────────
static std::wstring GetCrashLogDir() {
    WCHAR ap[MAX_PATH] = {};
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, ap);
    std::wstring dir = std::wstring(ap) + L"\\NexMatchAC_CrashLogs";
    CreateDirectoryW(dir.c_str(), NULL);
    return dir;
}
static std::wstring GetTimestampW() {
    SYSTEMTIME t; GetLocalTime(&t);
    WCHAR b[32];
    swprintf_s(b, L"%04d%02d%02d_%02d%02d%02d",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
static LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* pEx) {
    std::wstring file = GetCrashLogDir() + L"\\crash_" + GetTimestampW() + L".log";
    std::wofstream log(file.c_str());
    if (log) {
        log << L"Unhandled exception occurred.\n";
        if (pEx && pEx->ExceptionRecord) {
            log << L"ExceptionCode: 0x" << std::hex
                << (DWORD)pEx->ExceptionRecord->ExceptionCode << std::dec << L"\n";
            log << L"ExceptionAddress: " << pEx->ExceptionRecord->ExceptionAddress << L"\n";
        }
        log << L"ProcessID: " << GetCurrentProcessId() << L"\n";
    }
    MessageBoxW(NULL, L"Bir hata olustu. Crash dosyasi loglandi.",
        L"NexMatch Hata", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ── Log ────────────────────────────────────────────────────────────────────────
void AddLog(const std::wstring& icon, const std::wstring& msg) {
    if (!g_hLogList) return;
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::wstring entry = icon + L"  [" + TimeStr() + L"]  " + msg;
    SendMessageW(g_hLogList, LB_INSERTSTRING, 0, (LPARAM)entry.c_str());
    int cnt = (int)SendMessageW(g_hLogList, LB_GETCOUNT, 0, 0);
    while (cnt > 200) { SendMessageW(g_hLogList, LB_DELETESTRING, cnt - 1, 0); cnt--; }
}

// ── Tray tooltip ───────────────────────────────────────────────────────────────
void UpdateTrayTooltip() {
    if (!g_trayAdded) return;
    WCHAR tip[128];
    swprintf_s(tip, L"NexMatch Anti Cheat\n%s | Toplanan: %d | Gonderim: %d",
        g_cs2Running.load() ? L"AKTIF" : L"BEKLIYOR",
        g_ssCount.load(), g_uploadCount.load());
    wcsncpy_s(g_nid.szTip, tip, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ── Tray management ────────────────────────────────────────────────────────────
static void AddTrayIcon() {
    if (g_trayAdded) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = ID_TRAY_ICON;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    int traySize = GetSystemMetrics(SM_CXSMICON);
    HICON hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON,
        traySize, traySize, LR_DEFAULTCOLOR);
    if (!hIcon) hIcon = LoadIcon(NULL, IDI_APPLICATION);
    g_nid.hIcon = hIcon;
    wcscpy_s(g_nid.szTip, L"NexMatch Anti Cheat");
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    Sleep(100);
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
        g_trayAdded = true;
    }
}
static void EnsureTrayIcon() {
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    NOTIFYICONDATAW test = {};
    test.cbSize = sizeof(test); test.hWnd = g_hwnd;
    test.uID = ID_TRAY_ICON; test.uFlags = NIF_TIP;
    if (!Shell_NotifyIconW(NIM_MODIFY, &test)) { g_trayAdded = false; AddTrayIcon(); }
}
static void RemoveTrayIcon() {
    if (g_trayAdded) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_trayAdded = false; }
}
static void ShowTrayMenu() {
    POINT pt; GetCursorPos(&pt);
    HMENU hm = CreatePopupMenu();
    WCHAR status[64];
    swprintf_s(status, L"Durum: %s", g_cs2Running.load() ? L"CS2 Aktif" : L"Bekliyor");
    AppendMenuW(hm, MF_STRING | MF_GRAYED, 0, status);
    WCHAR ssInfo[64]; swprintf_s(ssInfo, L"Veri: %d", g_ssCount.load());
    AppendMenuW(hm, MF_STRING | MF_GRAYED, 0, ssInfo);
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING, ID_TRAY_SHOW, IsWindowVisible(g_hwnd) ? L"Gizle" : L"Goster");
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING, ID_TRAY_EXIT, L"Cikis Yap");
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(hm, TPM_BOTTOMALIGN | TPM_RIGHTALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(hm);
}

// ── Drawing helpers ────────────────────────────────────────────────────────────
static void FillR(HDC h, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c); FillRect(h, &r, b); DeleteObject(b);
}
static void DrawGradV(HDC h, RECT r, COLORREF c1, COLORREF c2) {
    int ht = r.bottom - r.top; if (ht <= 0) return;
    int r1=GetRValue(c1),g1=GetGValue(c1),b1=GetBValue(c1);
    int r2=GetRValue(c2),g2=GetGValue(c2),b2=GetBValue(c2);
    for (int y = 0; y < ht; ++y) {
        HPEN p = CreatePen(PS_SOLID, 1,
            RGB(r1+(r2-r1)*y/ht, g1+(g2-g1)*y/ht, b1+(b2-b1)*y/ht));
        HPEN o = (HPEN)SelectObject(h, p);
        MoveToEx(h, r.left, r.top+y, NULL); LineTo(h, r.right, r.top+y);
        SelectObject(h, o); DeleteObject(p);
    }
}
static void RBox(HDC h, RECT r, int rad, COLORREF fill, COLORREF brd, int bw = 1) {
    HBRUSH hb = CreateSolidBrush(fill); HPEN hp = CreatePen(PS_SOLID, bw, brd);
    HBRUSH ob = (HBRUSH)SelectObject(h, hb); HPEN op = (HPEN)SelectObject(h, hp);
    RoundRect(h, r.left, r.top, r.right, r.bottom, rad*2, rad*2);
    SelectObject(h, ob); SelectObject(h, op); DeleteObject(hb); DeleteObject(hp);
}
static void DLine(HDC h, int x1, int y1, int x2, int y2, COLORREF c, int w = 1) {
    HPEN p = CreatePen(PS_SOLID, w, c); HPEN o = (HPEN)SelectObject(h, p);
    MoveToEx(h, x1, y1, NULL); LineTo(h, x2, y2);
    SelectObject(h, o); DeleteObject(p);
}
static HFONT MkFont(int sz, int wt, const wchar_t* f, bool it = false) {
    return CreateFontW(-sz, 0, 0, 0, wt, it, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, f);
}
static void DrawCenteredText(HDC h, RECT r, const wchar_t* txt, COLORREF c, HFONT f) {
    HFONT of = (HFONT)SelectObject(h, f);
    SetBkMode(h, TRANSPARENT); SetTextColor(h, c);
    DrawTextW(h, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(h, of);
}

// ── Animated status dot ────────────────────────────────────────────────────────
static void DrawPulseDot(HDC h, int cx, int cy, bool active, float pulse) {
    if (active) {
        int glowR = 9 + (int)(3 * pulse);
        COLORREF glowC = RGB(0, (int)(30+30*pulse), (int)(15+15*pulse));
        HBRUSH hbg = CreateSolidBrush(glowC); HPEN hpg = CreatePen(PS_NULL, 0, 0);
        HBRUSH obg = (HBRUSH)SelectObject(h, hbg); HPEN opg = (HPEN)SelectObject(h, hpg);
        Ellipse(h, cx-glowR, cy-glowR, cx+glowR, cy+glowR);
        SelectObject(h, obg); SelectObject(h, opg); DeleteObject(hbg); DeleteObject(hpg);

        COLORREF dotC = RGB(0, (int)(170+40*pulse), (int)(88+22*pulse));
        HBRUSH hbd = CreateSolidBrush(dotC); HPEN hpd = CreatePen(PS_NULL, 0, 0);
        HBRUSH obd = (HBRUSH)SelectObject(h, hbd); HPEN opd = (HPEN)SelectObject(h, hpd);
        Ellipse(h, cx-5, cy-5, cx+5, cy+5);
        SelectObject(h, obd); SelectObject(h, opd); DeleteObject(hbd); DeleteObject(hpd);
    } else {
        HBRUSH hbd = CreateSolidBrush(C_BORDER2); HPEN hpd = CreatePen(PS_NULL, 0, 0);
        HBRUSH obd = (HBRUSH)SelectObject(h, hbd); HPEN opd = (HPEN)SelectObject(h, hpd);
        Ellipse(h, cx-5, cy-5, cx+5, cy+5);
        SelectObject(h, obd); SelectObject(h, opd); DeleteObject(hbd); DeleteObject(hpd);
    }
}

// ── Main window paint ──────────────────────────────────────────────────────────
static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    HDC mdc = CreateCompatibleDC(hdc);
    HBITMAP mbm = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP obm = (HBITMAP)SelectObject(mdc, mbm);

    bool run = g_cs2Running.load();
    int  ss  = g_ssCount.load();

    float pulse = (g_dotAnim < 30) ? g_dotAnim / 30.0f : (60 - g_dotAnim) / 30.0f;

    // Background
    FillR(mdc, {0, 0, W, H}, C_BG);

    // Header (y: 0–110)
    DrawGradV(mdc, {0, 0, W, 110}, C_PANEL2, C_BG);
    DLine(mdc, 0, 110, W, 110, C_BORDER2);

    COLORREF accentC = run ? C_GREEN : C_BORDER;
    FillR(mdc, {0, 0, 5, 110}, accentC);
    if (run) FillR(mdc, {5, 0, 9, 110}, RGB(0, (int)(50+20*pulse), (int)(25+10*pulse)));

    SetBkMode(mdc, TRANSPARENT);
    {
        HFONT f = MkFont(40, FW_BLACK, L"Bahnschrift");
        HFONT o = (HFONT)SelectObject(mdc, f);
        SIZE szN = {};
        SetTextColor(mdc, C_WHITE);
        GetTextExtentPoint32W(mdc, L"Nex", 3, &szN);
        TextOutW(mdc, 22, 15, L"Nex", 3);
        SetTextColor(mdc, run ? C_GREEN : C_GREEN_DIM);
        TextOutW(mdc, 22 + szN.cx, 15, L"Match", 5);
        SelectObject(mdc, o); DeleteObject(f);
    }
    {
        HFONT f = MkFont(11, FW_NORMAL, L"Segoe UI");
        HFONT o = (HFONT)SelectObject(mdc, f);
        SetTextColor(mdc, C_GRAY);
        TextOutW(mdc, 24, 65, L"ANTI CHEAT SYSTEM  \xB7  CS2 MONITOR", 33);
        SelectObject(mdc, o); DeleteObject(f);
    }
    {
        RECT vr = {W-82, 10, W-8, 30};
        RBox(mdc, vr, 5, C_PANEL, C_BORDER);
        HFONT f = MkFont(10, FW_SEMIBOLD, L"Segoe UI");
        DrawCenteredText(mdc, vr, L"v1.0", C_GRAY, f);
        DeleteObject(f);
    }
    {
        int dcx = W-24, dcy = 85;
        DrawPulseDot(mdc, dcx, dcy, run, pulse);
        HFONT fst = MkFont(12, FW_BOLD, L"Segoe UI");
        HFONT ost = (HFONT)SelectObject(mdc, fst);
        SetBkMode(mdc, TRANSPARENT);
        SetTextColor(mdc, run ? C_GREEN : C_GRAY);
        const wchar_t* stTxt = run ? L"KORUMA AKT\u0130F" : L"CS2 BEKLEN\u0130YOR";
        SIZE stSz = {}; GetTextExtentPoint32W(mdc, stTxt, (int)wcslen(stTxt), &stSz);
        TextOutW(mdc, dcx-14-stSz.cx, dcy-stSz.cy/2, stTxt, (int)wcslen(stTxt));
        SelectObject(mdc, ost); DeleteObject(fst);
    }

    // Status card (y: 120–213)
    {
        int cy = 120, ch = 93;
        COLORREF brdC = run ? RGB(0, 55, 32) : C_BORDER;
        RBox(mdc, {18, cy, W-18, cy+ch}, 10, C_PANEL, brdC);
        FillR(mdc, {24, cy+1, W-24, cy+5}, run ? C_GREEN : C_BORDER2);

        if (run) {
            HFONT fl = MkFont(9, FW_SEMIBOLD, L"Segoe UI");
            HFONT ol = (HFONT)SelectObject(mdc, fl);
            SetBkMode(mdc, TRANSPARENT); SetTextColor(mdc, C_GRAY);
            TextOutW(mdc, 32, cy+14, L"G\u00D6NDER\u0130LEN VER\u0130", 15);
            SelectObject(mdc, ol); DeleteObject(fl);
            {
                HFONT fv = MkFont(34, FW_BLACK, L"Bahnschrift");
                HFONT ov = (HFONT)SelectObject(mdc, fv);
                SetTextColor(mdc, C_WHITE);
                WCHAR uploadW[16]; swprintf_s(uploadW, L"%d", g_uploadCount.load());
                TextOutW(mdc, 32, cy+24, uploadW, (int)wcslen(uploadW));
                SIZE uploadS = {}; GetTextExtentPoint32W(mdc, uploadW, (int)wcslen(uploadW), &uploadS);
                SelectObject(mdc, ov); DeleteObject(fv);
                HFONT fvl = MkFont(11, FW_NORMAL, L"Segoe UI");
                HFONT ovl = (HFONT)SelectObject(mdc, fvl);
                SetTextColor(mdc, C_GRAY);
                TextOutW(mdc, 34+uploadS.cx, cy+42, L"g\u00F6nderim", 8);
                SelectObject(mdc, ovl); DeleteObject(fvl);
            }
            int bx = W/2+10, bw2 = W/2-38, by = cy+30, bh = 8;
            {
                HFONT ft = MkFont(9, FW_SEMIBOLD, L"Segoe UI");
                HFONT ot = (HFONT)SelectObject(mdc, ft);
                SetTextColor(mdc, C_GRAY);
                TextOutW(mdc, bx, cy+14, L"UPLOAD DURUMU", 13);
                SelectObject(mdc, ot); DeleteObject(ft);
            }
            RBox(mdc, {bx, by, bx+bw2, by+bh}, 4, RGB(10,12,18), C_BORDER);
            int fi = (int)(bw2 * (1.0f - (float)g_uploadCountdown / 30.0f));
            if (fi > 3) RBox(mdc, {bx, by, bx+fi, by+bh}, 4, C_GREEN, C_GREEN);
            {
                HFONT ft = MkFont(11, FW_NORMAL, L"Segoe UI");
                HFONT ot = (HFONT)SelectObject(mdc, ft);
                SetTextColor(mdc, C_TEXT_DIM);
                WCHAR tbuf[64]; int pending = 0;
                { std::lock_guard<std::mutex> lk(g_filesMutex); pending = (int)g_pendingFiles.size(); }
                swprintf_s(tbuf, L"Sonraki upload: %ds | Bekleyen: %d", g_uploadCountdown, pending);
                TextOutW(mdc, bx, by+14, tbuf, (int)wcslen(tbuf));
                SelectObject(mdc, ot); DeleteObject(ft);
            }
        } else {
            HFONT f1 = MkFont(14, FW_SEMIBOLD, L"Segoe UI");
            HFONT o1 = (HFONT)SelectObject(mdc, f1);
            SetBkMode(mdc, TRANSPARENT); SetTextColor(mdc, C_TEXT_DIM);
            TextOutW(mdc, 32, cy+20, L"Bekleme Modu", 12);
            SelectObject(mdc, o1); DeleteObject(f1);
            HFONT f2 = MkFont(11, FW_NORMAL, L"Segoe UI");
            HFONT o2 = (HFONT)SelectObject(mdc, f2);
            SetTextColor(mdc, C_GRAY);
            TextOutW(mdc, 32, cy+46,
                L"CS2 ba\u015flat\u0131ld\u0131\u011f\u0131nda izleme otomatik devreye girer.", 50);
            SelectObject(mdc, o2); DeleteObject(f2);
        }
    }

    // Info cards (y: 225–315)
    {
        int cy = 225, ch = 90, cw = (W-60)/3;
        std::wstring stStr    = run ? L"Aktif" : L"Pasif";
        std::wstring idStr    = (g_steamId.empty() || g_steamId == L"unknown") ? L"---" : g_steamId;

        struct CardDef { const wchar_t* label; std::wstring value; COLORREF valC, topC; };
        CardDef cards[3] = {
            { L"DURUM",              stStr,                   run ? C_GREEN : C_GRAY, run ? C_GREEN : C_BORDER },
            { L"TOPLANAN VER\u0130", std::to_wstring(ss),     C_WHITE,                C_BORDER2 },
            { L"STEAM ID",           idStr,                   C_TEXT,                 C_BORDER2 },
        };
        for (int i = 0; i < 3; i++) {
            int cx = 18 + i * (cw+12);
            RBox(mdc, {cx, cy, cx+cw, cy+ch}, 8, C_PANEL, C_BORDER);
            FillR(mdc, {cx+3, cy+1, cx+cw-3, cy+5}, cards[i].topC);
            {
                HFONT fl = MkFont(9, FW_SEMIBOLD, L"Segoe UI");
                HFONT ol = (HFONT)SelectObject(mdc, fl);
                SetBkMode(mdc, TRANSPARENT); SetTextColor(mdc, C_GRAY);
                TextOutW(mdc, cx+14, cy+13, cards[i].label, (int)wcslen(cards[i].label));
                SelectObject(mdc, ol); DeleteObject(fl);
            }
            int valSz = (i == 2) ? 12 : 24;
            HFONT fv = MkFont(valSz, FW_BOLD, L"Bahnschrift");
            HFONT ov = (HFONT)SelectObject(mdc, fv);
            SetTextColor(mdc, cards[i].valC);
            std::wstring displayVal = cards[i].value;
            if (i == 2 && displayVal.size() > 14) displayVal = displayVal.substr(0, 14) + L"...";
            TextOutW(mdc, cx+14, cy+32, displayVal.c_str(), (int)displayVal.size());
            SelectObject(mdc, ov); DeleteObject(fv);
        }
    }

    // Log header
    DLine(mdc, 18, 328, W-18, 328, C_BORDER);
    {
        HFONT f = MkFont(10, FW_SEMIBOLD, L"Segoe UI");
        HFONT o = (HFONT)SelectObject(mdc, f);
        SetBkMode(mdc, TRANSPARENT); SetTextColor(mdc, C_GRAY);
        TextOutW(mdc, 18, 337, L"S\u0130STEM G\u00DCNL\u00DC\u011e\u00dc", 15);
        int logCnt = (int)SendMessageW(g_hLogList, LB_GETCOUNT, 0, 0);
        WCHAR lbuf[32]; swprintf_s(lbuf, L"%d kay\u0131t", logCnt);
        SIZE ls; GetTextExtentPoint32W(mdc, lbuf, (int)wcslen(lbuf), &ls);
        SetTextColor(mdc, C_BORDER2);
        TextOutW(mdc, W-18-ls.cx, 337, lbuf, (int)wcslen(lbuf));
        SelectObject(mdc, o); DeleteObject(f);
    }

    // Footer
    FillR(mdc, {0, H-48, W, H}, C_PANEL);
    DLine(mdc, 0, H-48, W, H-48, C_BORDER);
    {
        HFONT f = MkFont(10, FW_NORMAL, L"Segoe UI");
        HFONT o = (HFONT)SelectObject(mdc, f);
        SetBkMode(mdc, TRANSPARENT);
        if (!g_pcName.empty()) {
            SetTextColor(mdc, C_TEXT_DIM);
            std::wstring pcInfo = L"PC: " + g_pcName;
            TextOutW(mdc, 18, H-30, pcInfo.c_str(), (int)pcInfo.size());
        }
        SetTextColor(mdc, C_BORDER2);
        TextOutW(mdc, W-102, H-30, L"NexMatch v1.0", 13);
        SelectObject(mdc, o); DeleteObject(f);
    }

    BitBlt(hdc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, obm); DeleteObject(mbm); DeleteDC(mdc);
    EndPaint(hwnd, &ps);
}

// ── Controls ───────────────────────────────────────────────────────────────────
static void CreateControls(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    g_hLogList = CreateWindowExW(0, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_NOSEL,
        18, 358, rc.right-36, rc.bottom-410,
        hwnd, (HMENU)ID_LOG_LIST, g_hInst, NULL);
    HFONT fLog = MkFont(11, FW_NORMAL, L"Consolas");
    SendMessageW(g_hLogList, WM_SETFONT, (WPARAM)fLog, TRUE);
}

// ── EULA ───────────────────────────────────────────────────────────────────────
static const wchar_t* EULA_TEXT =
L"Bu yazilimi kullanmadan once asagidaki sozlesmeyi dikkatlice okuyunuz.\r\n\r\n"
L"1. TOPLANAN VERILER\r\n"
L"Bu uygulama, CS2 oyunu aktifken asagidaki verileri toplar:\r\n"
L"  \x2022  Ekran bilgileri (rastgele araliklarla)\r\n"
L"  \x2022  Steam ID (64-bit)\r\n"
L"  \x2022  Bilgisayar adi (hostname)\r\n"
L"  \x2022  Zaman damgasi (tarih/saat)\r\n\r\n"
L"2. VERILERIN KULLANIMI\r\n"
L"Toplanan veriler yalnizca hile tespiti amaciyla kullanilir ve\r\n"
L"NexMatch sunucularinda guvenli sekilde saklanir.\r\n"
L"Veriler ucuncu kisilerle paylasilmaz.\r\n\r\n"
L"3. EKRAN BILGISI\r\n"
L"Uygulama, CS2 oturumunuz boyunca belirli araliklarla otomatik\r\n"
L"ekran bilgileri alir ve sunucuya iletir. Gonderim sonrasinda\r\n"
L"yerel veri otomatik olarak silinir.\r\n\r\n"
L"4. ONAY\r\n"
L"'Kabul Ediyorum' butonuna basarak yukardaki kosullari\r\n"
L"okudugunuzu ve kabul ettiginizi beyan etmis olursunuz.\r\n"
L"Kabul etmemeniz durumunda uygulama kapatilacaktir.";

static void DrawEulaButton(HDC hdc, RECT r, const wchar_t* text, bool isPrimary, bool hover) {
    COLORREF fill, border, textC;
    if (isPrimary) {
        fill   = hover ? RGB(0,230,120) : C_GREEN;
        border = hover ? RGB(0,255,140) : C_GREEN_DIM;
        textC  = RGB(5,15,10);
    } else {
        fill   = hover ? RGB(36,42,60) : C_PANEL;
        border = hover ? C_BORDER2 : C_BORDER;
        textC  = hover ? C_WHITE : C_TEXT_DIM;
    }
    RBox(hdc, r, 7, fill, border, 2);
    SetBkMode(hdc, TRANSPARENT);
    HFONT f = MkFont(13, FW_SEMIBOLD, L"Segoe UI");
    DrawCenteredText(hdc, r, text, textC, f);
    DeleteObject(f);
}

LRESULT CALLBACK EulaWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        RECT crc; GetClientRect(hwnd, &crc);
        HWND hEdit = CreateWindowExW(0, L"EDIT", EULA_TEXT,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            20, 100, crc.right-40, crc.bottom-178,
            hwnd, (HMENU)ID_EULA_TEXT, g_hInst, NULL);
        HFONT fEdit = MkFont(12, FW_NORMAL, L"Segoe UI");
        SendMessageW(hEdit, WM_SETFONT, (WPARAM)fEdit, TRUE);
        SetTimer(hwnd, 99, 30, NULL);
        return 0;
    }
    case WM_TIMER: {
        if (wParam == 99) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            RECT rc2; GetClientRect(hwnd, &rc2);
            int W = rc2.right, H = rc2.bottom;
            RECT ra={W-310,H-56,W-170,H-18}, rd={W-160,H-56,W-12,H-18};
            bool ha = PtInRect(&ra, &pt) != FALSE;
            bool hd = PtInRect(&rd, &pt) != FALSE;
            if (ha != g_eulaHoverAccept || hd != g_eulaHoverDecline) {
                g_eulaHoverAccept = ha; g_eulaHoverDecline = hd;
                RECT br = {0, H-72, W, H}; InvalidateRect(hwnd, &br, FALSE);
            }
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc2; GetClientRect(hwnd, &rc2);
        int W = rc2.right, H = rc2.bottom;
        HDC mdc = CreateCompatibleDC(hdc);
        HBITMAP mbm = CreateCompatibleBitmap(hdc, W, H);
        HBITMAP obm = (HBITMAP)SelectObject(mdc, mbm);

        FillR(mdc, {0,0,W,H}, C_BG);
        DrawGradV(mdc, {0,0,W,92}, C_PANEL2, C_BG);
        DLine(mdc, 0, 92, W, 92, C_BORDER2);
        FillR(mdc, {0,0,5,92}, C_GREEN);
        SetBkMode(mdc, TRANSPARENT);
        {
            HFONT fT = MkFont(28, FW_BLACK, L"Bahnschrift");
            HFONT oT = (HFONT)SelectObject(mdc, fT);
            SIZE szN = {};
            SetTextColor(mdc, C_WHITE);
            GetTextExtentPoint32W(mdc, L"Nex", 3, &szN);
            TextOutW(mdc, 18, 14, L"Nex", 3);
            SetTextColor(mdc, C_GREEN);
            TextOutW(mdc, 18+szN.cx, 14, L"Match", 5);
            SelectObject(mdc, oT); DeleteObject(fT);
        }
        {
            HFONT fS = MkFont(11, FW_NORMAL, L"Segoe UI");
            HFONT oS = (HFONT)SelectObject(mdc, fS);
            SetTextColor(mdc, C_GRAY);
            TextOutW(mdc, 20, 52, L"KULLANICI VE G\u0130ZL\u0130L\u0130K S\u00d6ZLESMES\u0130", 33);
            SelectObject(mdc, oS); DeleteObject(fS);
        }
        {
            RECT vb = {W-100,15,W-12,36};
            RBox(mdc, vb, 5, C_PANEL, C_BORDER);
            HFONT fb = MkFont(10, FW_SEMIBOLD, L"Segoe UI");
            DrawCenteredText(mdc, vb, L"Surum v1.0", C_GRAY, fb);
            DeleteObject(fb);
        }
        FillR(mdc, {0,H-70,W,H}, C_PANEL2);
        DLine(mdc, 0, H-70, W, H-70, C_BORDER2);
        {
            HFONT fI = MkFont(11, FW_NORMAL, L"Segoe UI", true);
            HFONT oI = (HFONT)SelectObject(mdc, fI);
            SetTextColor(mdc, C_GRAY);
            TextOutW(mdc, 18, H-52, L"Devam etmek icin sozlesmeyi kabul etmelisiniz.", 46);
            SelectObject(mdc, oI); DeleteObject(fI);
        }
        RECT ra={W-310,H-56,W-170,H-18}, rd={W-160,H-56,W-12,H-18};
        DrawEulaButton(mdc, ra, L"Kabul Ediyorum", true,  g_eulaHoverAccept);
        DrawEulaButton(mdc, rd, L"Reddediyorum",   false, g_eulaHoverDecline);

        BitBlt(hdc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, obm); DeleteObject(mbm); DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hd = (HDC)wParam;
        SetBkMode(hd, OPAQUE); SetBkColor(hd, RGB(14,17,25)); SetTextColor(hd, C_TEXT);
        static HBRUSH hbEdit = CreateSolidBrush(RGB(14,17,25));
        return (LRESULT)hbEdit;
    }
    case WM_LBUTTONDOWN: {
        RECT rc2; GetClientRect(hwnd, &rc2);
        int W = rc2.right, H = rc2.bottom;
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT ra={W-310,H-56,W-170,H-18}, rd={W-160,H-56,W-12,H-18};
        if (PtInRect(&ra, &pt)) { KillTimer(hwnd,99); g_eulaAccepted=true;  g_eulaFinished=true; DestroyWindow(hwnd); return 0; }
        if (PtInRect(&rd, &pt)) { KillTimer(hwnd,99); g_eulaAccepted=false; g_eulaFinished=true; DestroyWindow(hwnd); return 0; }
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, 99); g_eulaAccepted=false; g_eulaFinished=true; DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool ShowEulaDialog() {
    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = EulaWndProc; wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"NexMatchEULA";
    wc.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_hEulaWnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"NexMatchEULA", L"NexMatch Anti Cheat - Kullanici Sozlesmesi",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        (sw-EULA_W)/2, (sh-EULA_H)/2, EULA_W, EULA_H,
        NULL, NULL, g_hInst, NULL);
    if (!g_hEulaWnd) return false;

    BOOL dark = TRUE; DwmSetWindowAttribute(g_hEulaWnd, 20, &dark, sizeof(dark));
    ShowWindow(g_hEulaWnd, SW_SHOW); UpdateWindow(g_hEulaWnd);
    g_eulaFinished = false;
    MSG msg;
    while (!g_eulaFinished && GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return g_eulaAccepted;
}

// ── Main window proc ───────────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HBRUSH s_bg   = CreateSolidBrush(C_BG);
    static HBRUSH s_pan2 = CreateSolidBrush(C_PANEL2);

    switch (msg) {
    case WM_CREATE: {
        CreateControls(hwnd);
        AddTrayIcon();
        SetTimer(hwnd, ID_TIMER_CHECK,  2000, NULL);
        SetTimer(hwnd, ID_TIMER_UPLOAD, 1000, NULL);
        SetTimer(hwnd, ID_TIMER_PULSE,    60, NULL);
        SetTimer(hwnd, ID_TIMER_TRAY,   5000, NULL);
        AddLog(L">>", L"NexMatch Anti Cheat v1 baslatildi.");
        g_steamId = GetSteamID(); g_pcName = GetPCName();
        if (g_steamId != L"unknown") AddLog(L"OK", L"Steam ID alindi: " + g_steamId);
        else AddLog(L"--", L"Steam aktif degil, Steam ID alinamadi.");
        AddLog(L">>", L"CS2 sureci bekleniyor...");
        return 0;
    }
    case WM_CLOSE: {
        if (!g_realClose) {
            ShowWindow(hwnd, SW_HIDE);
            EnsureTrayIcon();
            static bool shown = false;
            if (!shown) {
                shown = true;
                g_nid.uFlags |= NIF_INFO;
                wcscpy_s(g_nid.szInfoTitle, L"NexMatch calismayi surduruyor");
                wcscpy_s(g_nid.szInfo, L"Sistem tepsisinden erisebilirsiniz.");
                g_nid.dwInfoFlags = NIIF_INFO;
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
                g_nid.uFlags &= ~NIF_INFO;
            }
            return 0;
        }
        KillTimer(hwnd, ID_TIMER_CHECK);
        KillTimer(hwnd, ID_TIMER_UPLOAD);
        KillTimer(hwnd, ID_TIMER_PULSE);
        KillTimer(hwnd, ID_TIMER_TRAY);
        RemoveTrayIcon();
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0); return 0;

    case WM_TRAYICON: {
        UINT ev = LOWORD(lParam);
        if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU)
            ShowTrayMenu();
        else if (ev == WM_LBUTTONDBLCLK || ev == NIN_SELECT) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            else { ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_TRAY_SHOW) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            else { ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
        } else if (id == ID_TRAY_EXIT) {
            g_realClose = true; SendMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    case WM_TIMER: {
        if (wParam == ID_TIMER_TRAY)
            EnsureTrayIcon();

        if (wParam == ID_TIMER_PULSE) {
            g_dotAnim = (g_dotAnim + 1) % 60;
            RECT hr = {0, 0, 1200, 220}; InvalidateRect(hwnd, &hr, FALSE);
        }

        if (wParam == ID_TIMER_CHECK) {
            bool was = g_cs2Running.load(), now = IsCS2Running();
            if (now && !was) {
                g_cs2Running = true;
                g_sessionFolder = CreateSessionFolder();
                g_sessionActive = true; g_ssCount = 0; g_uploadCount = 0;
                g_screenshotCountdown = 10; g_uploadCountdown = 30;
                { std::lock_guard<std::mutex> lk(g_filesMutex); g_pendingFiles.clear(); }
                AddLog(L"OK", L"CS2 algilandi. Koruma aktif.");
                AddLog(L">>", L"Veri toplama basladi.");
                InvalidateRect(hwnd, NULL, FALSE);
                UpdateTrayTooltip();
            } else if (!now && was) {
                g_cs2Running = false; g_sessionActive = false;
                WCHAR buf[64]; swprintf_s(buf, L"Oturum sona erdi. Toplam veri: %d", g_ssCount.load());
                AddLog(L"--", buf);
                InvalidateRect(hwnd, NULL, FALSE);
                UpdateTrayTooltip();
            }
        }

        if (wParam == ID_TIMER_UPLOAD) {
            g_heartbeatCountdown--;
            if (g_heartbeatCountdown <= 0) { TriggerHeartbeat(); g_heartbeatCountdown = 30; }
        }

        if (wParam == ID_TIMER_UPLOAD && g_cs2Running && g_sessionActive) {
            g_screenshotCountdown--;
            if (g_screenshotCountdown <= 0) {
                std::wstring filePath = SaveScreenshotToFile();
                if (!filePath.empty()) {
                    std::lock_guard<std::mutex> lk(g_filesMutex);
                    g_pendingFiles.push_back(filePath);
                    g_ssCount++;
                }
                g_screenshotCountdown = 10;
            }
            g_uploadCountdown--;
            if (g_uploadCountdown <= 0) { TriggerUpload(); g_uploadCountdown = 30; }

            RECT cr = {0,115,1200,225}; InvalidateRect(hwnd, &cr, FALSE);
            RECT lr = {0,324,1200,360}; InvalidateRect(hwnd, &lr, FALSE);
        }
        return 0;
    }
    case WM_PAINT:
        OnPaint(hwnd); return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLORLISTBOX: {
        HDC hd = (HDC)wParam;
        SetBkColor(hd, RGB(13,15,23)); SetTextColor(hd, C_TEXT);
        return (LRESULT)CreateSolidBrush(RGB(13,15,23));
    }
    case WM_CTLCOLORSTATIC: {
        HDC hd = (HDC)wParam;
        SetBkMode(hd, TRANSPARENT); SetTextColor(hd, C_TEXT_DIM);
        return (LRESULT)s_bg;
    }
    case WM_CTLCOLORBTN:
        return (LRESULT)s_pan2;

    case WM_SIZE: {
        RECT r2; GetClientRect(hwnd, &r2);
        if (g_hLogList)
            SetWindowPos(g_hLogList, NULL, 18, 358, r2.right-36, r2.bottom-410, SWP_NOZORDER);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize = {520, 600};
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── Entry point ────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    g_hInst = hInst;
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"NexMatchAC_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"NexMatch zaten calisiyor.", L"NexMatch", MB_ICONINFORMATION);
        return 0;
    }
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);

    GdiplusStartupInput gsi; GdiplusStartup(&g_gdipToken, &gsi, NULL);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    SetProcessDPIAware();

    if (!ShowEulaDialog()) {
        MessageBoxW(NULL, L"Sozlesme reddedildi. Uygulama kapatiliyor.",
            L"NexMatch", MB_ICONINFORMATION);
        GdiplusShutdown(g_gdipToken); CloseHandle(hMutex); return 0;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"NexMatchAC_v1";
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    const int WW = 720, WH = 720;
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW,
        L"NexMatchAC_v1", L"NexMatch Anti Cheat",
        WS_OVERLAPPEDWINDOW,
        (GetSystemMetrics(SM_CXSCREEN)-WW)/2,
        (GetSystemMetrics(SM_CYSCREEN)-WH)/2,
        WW, WH, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return -1;

    SendMessageW(g_hwnd, WM_SETICON, ICON_BIG,   (LPARAM)wc.hIcon);
    SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL,  (LPARAM)wc.hIconSm);

    BOOL dark = TRUE; DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark));
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG message;
    while (GetMessage(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    CloseHandle(hMutex);
    GdiplusShutdown(g_gdipToken);
    return (int)message.wParam;
}
