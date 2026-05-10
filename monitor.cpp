/*==============================================================================
 * ProcessMonitor.cpp  -  Professional Windows Process Monitor
 *
 * ARCHITECTURE (for the next developer)
 * ======================================
 *  Data Layer:
 *    ProcessInfo    - Per-PID snapshot: CPU %, RAM, Peak RAM, Private Mem
 *    ProcessGroup   - All PIDs sharing one exe name, with aggregated totals
 *    g_prevSnapshot - std::unordered_map<DWORD,ProcessInfo> kept between
 *                     refreshes to compute CPU deltas
 *
 *  UI Layer:
 *    g_hList   - SysListView32 (LVS_REPORT + LVS_OWNERDATA-free + groups)
 *    g_hStatus - MSCTLS_STATUSBAR32 showing totals
 *    g_hSearch - Edit control for live name filtering
 *    Custom WM_PAINT on main window draws the dark header banner
 *
 *  Refresh Loop:
 *    WM_TIMER (ID_TIMER) fires every REFRESH_MS
 *    -> CollectSnapshots()  builds g_groups from a Toolhelp32 snapshot
 *    -> RebuildListView()   clears and repopulates the ListView with groups
 *    -> UpdateStatus()      pushes totals to the status bar
 *
 *  CPU Calculation:
 *    cpuPct = ((deltaKernel + deltaUser) * 100ns) / (wallSeconds * cores) * 100
 *    First sample always returns 0 % - correct on second tick.
 *
 *  Memory Display:
 *    FormatBytes() auto-scales to B / KB / MB / GB.
 *
 *  Extending:
 *    - Add a field to ProcessInfo, populate in CollectSnapshots()
 *    - Add an entry to COLUMNS[], render the value in RebuildListView()
 *    - For new right-click actions, add cases to the WM_CONTEXTMENU handler
 *==============================================================================
 */

#define UNICODE
#define _UNICODE
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <algorithm>
#include <commctrl.h>
#include <cwctype>
#include <iomanip>
#include <map>
#include <psapi.h>
#include <sstream>
#include <string>
#include <tlhelp32.h>
#include <unordered_map>
#include <uxtheme.h>
#include <vector>
#include <windows.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

// ── IDs ──────────────────────────────────────────────────────────
static const int ID_TIMER = 1;
static const int IDC_LIST = 101;
static const int IDC_STATUS = 102;
static const int IDC_SEARCH = 103;
static const int IDC_KILL = 104;
static const int REFRESH_MS = 2000;
static const int HDR_H = 64;  // header banner height (px)
static const int TOOL_H = 40; // toolbar strip height (px)

// ── Palette ───────────────────────────────────────────────────────
namespace C {
COLORREF BG = RGB(30, 30, 30);        // Professional dark gray
COLORREF PANEL = RGB(45, 45, 48);     // VS Dark panel
COLORREF ACCENT = RGB(0, 122, 204);   // Clean blue accent
COLORREF TEXT = RGB(241, 241, 241);   // White text
COLORREF DIMTXT = RGB(153, 153, 153); // Gray text
COLORREF ROWALT = RGB(37, 37, 38);    // Alternate row
COLORREF SEL = RGB(0, 122, 204);      // Selection blue
COLORREF GRPBG = RGB(45, 45, 48);
} // namespace C

// ── Data structures ───────────────────────────────────────────────

// One process's complete measured snapshot.
// Add new metrics here; populate them in CollectSnapshots().
struct ProcessInfo {
  DWORD pid = 0;
  std::wstring name;       // e.g. L"chrome.exe"
  double cpuPct = 0;       // 0 – 100 %
  SIZE_T ramBytes = 0;     // Working Set
  SIZE_T peakRamBytes = 0; // Peak Working Set
  SIZE_T privBytes = 0;    // Private Bytes
  // Internal: previous sample times for CPU delta
  ULONGLONG prevKernel = 0;
  ULONGLONG prevUser = 0;
  LONGLONG prevTick = 0; // QueryPerformanceCounter tick
};

// All PIDs that share the same executable name.
struct ProcessGroup {
  std::wstring name;
  std::vector<ProcessInfo> members;
  double totalCpu = 0;
  SIZE_T totalRam = 0;
};

// ── Globals ───────────────────────────────────────────────────────
static HWND g_hWnd = nullptr;
static HWND g_hList = nullptr;
static HWND g_hStatus = nullptr;
static HWND g_hSearch = nullptr;
static HWND g_hKill = nullptr;
static HFONT g_hFont = nullptr;
static HFONT g_hFontBd = nullptr;
static int g_cores = 1;
static std::unordered_map<DWORD, ProcessInfo> g_prev; // CPU delta store
static std::vector<ProcessGroup> g_groups;
static std::wstring g_filter; // live search filter

// ── Helpers ───────────────────────────────────────────────────────

std::wstring FormatBytes(SIZE_T b) {
  wchar_t buf[64];
  if (b >= (SIZE_T)1 << 30)
    swprintf_s(buf, L"%.2f GB", b / 1073741824.0);
  else if (b >= (SIZE_T)1 << 20)
    swprintf_s(buf, L"%.1f MB", b / 1048576.0);
  else if (b >= (SIZE_T)1 << 10)
    swprintf_s(buf, L"%.0f KB", b / 1024.0);
  else
    swprintf_s(buf, L"%zu B", b);
  return buf;
}

COLORREF CpuColor(double p) {
  if (p < 5)
    return C::TEXT; // Normal for low usage
  if (p < 25)
    return RGB(255, 215, 0); // Gold
  if (p < 60)
    return RGB(255, 140, 0); // Orange
  return RGB(255, 80, 80);   // Soft Red
}

inline ULONGLONG FT2ULL(const FILETIME &f) {
  return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

// ── CPU delta ─────────────────────────────────────────────────────
double CpuPercent(const ProcessInfo &cur, const ProcessInfo &prev) {
  if (prev.prevTick == 0)
    return 0.0;
  static LARGE_INTEGER qpf{};
  if (!qpf.QuadPart)
    QueryPerformanceFrequency(&qpf);
  double wall = (double)(cur.prevTick - prev.prevTick) / qpf.QuadPart;
  if (wall <= 0)
    return 0.0;
  ULONGLONG dt =
      (cur.prevKernel - prev.prevKernel) + (cur.prevUser - prev.prevUser);
  return std::min(100.0, (dt * 1e-7) / (wall * g_cores) * 100.0);
}

// ── Data collection ───────────────────────────────────────────────
void CollectSnapshots() {
  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnap == INVALID_HANDLE_VALUE)
    return;

  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  std::unordered_map<DWORD, ProcessInfo> newSnap;
  std::map<std::wstring, ProcessGroup> groupMap;

  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(hSnap, &pe)) {
    do {
      ProcessInfo info;
      info.pid = pe.th32ProcessID;
      info.name = pe.szExeFile;

      HANDLE hp = OpenProcess(
          PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, info.pid);
      if (hp) {
        // Memory
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(hp, (PROCESS_MEMORY_COUNTERS *)&pmc,
                                 sizeof(pmc))) {
          info.ramBytes = pmc.WorkingSetSize;
          info.peakRamBytes = pmc.PeakWorkingSetSize;
          info.privBytes = pmc.PrivateUsage;
        }
        // CPU
        FILETIME cr, ex, ker, usr;
        if (GetProcessTimes(hp, &cr, &ex, &ker, &usr)) {
          info.prevKernel = FT2ULL(ker);
          info.prevUser = FT2ULL(usr);
          info.prevTick = now.QuadPart;
          auto it = g_prev.find(info.pid);
          if (it != g_prev.end())
            info.cpuPct = CpuPercent(info, it->second);
        }
        CloseHandle(hp);
      }
      newSnap[info.pid] = info;

      // Group key = lowercase exe name
      std::wstring key = info.name;
      for (auto &ch : key)
        ch = towlower(ch);
      groupMap[key].name = info.name;
      groupMap[key].members.push_back(info);
    } while (Process32NextW(hSnap, &pe));
  }
  CloseHandle(hSnap);

  // Aggregate
  std::vector<ProcessGroup> groups;
  for (auto &[k, g] : groupMap) {
    g.totalCpu = 0;
    g.totalRam = 0;
    for (auto &p : g.members) {
      g.totalCpu += p.cpuPct;
      g.totalRam += p.ramBytes;
    }
    // Sort sub-items by RAM descending
    std::sort(g.members.begin(), g.members.end(),
              [](const ProcessInfo &a, const ProcessInfo &b) {
                return a.ramBytes > b.ramBytes;
              });
    groups.push_back(std::move(g));
  }
  // Default sort: RAM descending
  std::sort(groups.begin(), groups.end(),
            [](const ProcessGroup &a, const ProcessGroup &b) {
              return a.totalRam > b.totalRam;
            });

  g_groups = std::move(groups);
  g_prev = std::move(newSnap);
}

// ── Columns ───────────────────────────────────────────────────────
struct ColDef {
  const wchar_t *title;
  int cx;
  int fmt;
};
static const ColDef COLS[] = {
    {L"Process Name", 210, LVCFMT_LEFT}, {L"PID", 65, LVCFMT_RIGHT},
    {L"CPU %", 80, LVCFMT_RIGHT},        {L"Memory", 105, LVCFMT_RIGHT},
    {L"Peak Memory", 115, LVCFMT_RIGHT}, {L"Private Mem", 115, LVCFMT_RIGHT},
};
static const int NCOLS = (int)(sizeof(COLS) / sizeof(COLS[0]));

void SetupColumns() {
  for (int i = 0; i < NCOLS; ++i) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.pszText = (LPWSTR)COLS[i].title;
    c.cx = COLS[i].cx;
    c.fmt = COLS[i].fmt;
    ListView_InsertColumn(g_hList, i, &c);
  }
}

// ── List rebuild ──────────────────────────────────────────────────
void RebuildListView() {
  SendMessage(g_hList, WM_SETREDRAW, FALSE, 0);

  // Save current expanded/collapsed states
  std::unordered_map<std::wstring, UINT> groupStates;
  for (int i = 0; i < 2000; ++i) {
    LVGROUP lvg{};
    lvg.cbSize = sizeof(lvg);
    lvg.mask = LVGF_HEADER | LVGF_STATE;
    lvg.stateMask = LVGS_COLLAPSED;
    wchar_t buf[256] = {};
    lvg.pszHeader = buf;
    lvg.cchHeader = 256;
    if (SendMessage(g_hList, LVM_GETGROUPINFO, i, (LPARAM)&lvg) != -1) {
      std::wstring head = buf;
      size_t pos = head.find(L"  (");
      if (pos != std::wstring::npos)
        head = head.substr(0, pos);
      groupStates[head] = (lvg.state & LVGS_COLLAPSED);
    }
  }

  ListView_DeleteAllItems(g_hList);
  ListView_RemoveAllGroups(g_hList);

  int gid = 0, idx = 0;
  SIZE_T totRam = 0;
  double totCpu = 0;

  for (auto &grp : g_groups) {
    // Apply search filter
    std::wstring nameLo = grp.name;
    for (auto &ch : nameLo)
      ch = towlower(ch);
    if (!g_filter.empty() && nameLo.find(g_filter) == std::wstring::npos)
      continue;

    totRam += grp.totalRam;
    totCpu += grp.totalCpu;

    // Group header
    wchar_t hdr[300];
    if (grp.members.size() > 1)
      swprintf_s(hdr, L"%s  (%zu)   %s   %.1f%% CPU", grp.name.c_str(),
                 grp.members.size(), FormatBytes(grp.totalRam).c_str(),
                 grp.totalCpu);
    else
      swprintf_s(hdr, L"%s", grp.name.c_str());

    LVGROUP lvg{};
    lvg.cbSize = sizeof(lvg);
    lvg.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
    lvg.pszHeader = hdr;
    lvg.iGroupId = gid;
    lvg.state = LVGS_COLLAPSIBLE | LVGS_COLLAPSED; // Default collapsed

    // Restore previous state if known
    std::wstring baseName = grp.name;
    if (groupStates.find(baseName) != groupStates.end()) {
      lvg.state = LVGS_COLLAPSIBLE | groupStates[baseName];
    }

    lvg.stateMask = LVGS_COLLAPSIBLE | LVGS_COLLAPSED;
    ListView_InsertGroup(g_hList, -1, &lvg);

    for (auto &p : grp.members) {
      wchar_t cpu[32], ram[32], peak[32], priv[32], pid[16];
      swprintf_s(cpu, L"%.1f%%", p.cpuPct);
      swprintf_s(pid, L"%lu", p.pid);
      wcscpy_s(ram, FormatBytes(p.ramBytes).c_str());
      wcscpy_s(peak, FormatBytes(p.peakRamBytes).c_str());
      wcscpy_s(priv, FormatBytes(p.privBytes).c_str());

      LVITEMW item{};
      item.mask = LVIF_TEXT | LVIF_GROUPID;
      item.iItem = idx;
      item.iSubItem = 0;
      item.pszText = (LPWSTR)p.name.c_str();
      item.iGroupId = gid;
      ListView_InsertItem(g_hList, &item);
      ListView_SetItemText(g_hList, idx, 1, pid);
      ListView_SetItemText(g_hList, idx, 2, cpu);
      ListView_SetItemText(g_hList, idx, 3, ram);
      ListView_SetItemText(g_hList, idx, 4, peak);
      ListView_SetItemText(g_hList, idx, 5, priv);
      ++idx;
    }
    ++gid;
  }

  SendMessage(g_hList, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g_hList, nullptr, FALSE);

  // Status bar
  wchar_t sb[200];
  swprintf_s(sb,
             L"  %d processes   |   Total RAM: %s   |   Total CPU: %.1f%%   |  "
             L" Auto-refresh: %ds",
             idx, FormatBytes(totRam).c_str(), totCpu, REFRESH_MS / 1000);
  SetWindowTextW(g_hStatus, sb);
}

// ── Custom draw (dark theme) ──────────────────────────────────────
LRESULT HandleCustomDraw(LPARAM lp) {
  auto *cd = (LPNMLVCUSTOMDRAW)lp;
  switch (cd->nmcd.dwDrawStage) {
  case CDDS_PREPAINT:
    return CDRF_NOTIFYITEMDRAW;

  case CDDS_ITEMPREPAINT: {
    int row = (int)cd->nmcd.dwItemSpec;
    COLORREF bg = (row % 2 == 0) ? C::BG : C::ROWALT;
    // check selection
    UINT state = ListView_GetItemState(g_hList, row, LVIS_SELECTED);
    if (state & LVIS_SELECTED)
      bg = C::SEL;
    cd->clrTextBk = bg;
    cd->clrText = C::TEXT;
    return CDRF_NOTIFYSUBITEMDRAW | CDRF_NEWFONT;
  }
  case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
    int row = (int)cd->nmcd.dwItemSpec;
    UINT state = ListView_GetItemState(g_hList, row, LVIS_SELECTED);
    COLORREF bg = (state & LVIS_SELECTED) ? C::SEL
                  : (row % 2 == 0)        ? C::BG
                                          : C::ROWALT;
    cd->clrTextBk = bg;
    // CPU column gets heat-map color
    if (cd->iSubItem == 2) {
      wchar_t buf[32];
      ListView_GetItemText(g_hList, row, 2, buf, 32);
      double v = _wtof(buf);
      cd->clrText = CpuColor(v);
    } else if (cd->iSubItem == 3) {
      cd->clrText = RGB(126, 232, 162); // soft green for RAM
    } else {
      cd->clrText = C::TEXT;
    }
    return CDRF_NEWFONT;
  }
  }
  return CDRF_DODEFAULT;
}

// ── Header banner paint ───────────────────────────────────────────
void PaintHeader(HWND hWnd) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hWnd, &ps);
  RECT rc{};
  GetClientRect(hWnd, &rc);
  rc.bottom = HDR_H;
  // gradient: left = #0D1117 right = #1a2233
  for (int x = rc.left; x < rc.right; ++x) {
    double t = (double)x / std::max(1L, rc.right - rc.left);
    COLORREF col =
        RGB((BYTE)(13 + t * 13), (BYTE)(17 + t * 18), (BYTE)(23 + t * 34));
    HPEN p = CreatePen(PS_SOLID, 1, col);
    SelectObject(dc, p);
    MoveToEx(dc, x, rc.top, nullptr);
    LineTo(dc, x, rc.bottom);
    DeleteObject(p);
  }
  // Accent line at bottom of header
  HPEN ap = CreatePen(PS_SOLID, 2, C::ACCENT);
  SelectObject(dc, ap);
  MoveToEx(dc, 0, HDR_H - 1, nullptr);
  LineTo(dc, rc.right, HDR_H - 1);
  DeleteObject(ap);

  // Title text
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_hFontBd);
  SetTextColor(dc, C::TEXT);
  RECT tr = {16, 10, 500, HDR_H - 4};
  DrawTextW(dc, L"Process Monitor", -1, &tr,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  // Subtitle
  SelectObject(dc, g_hFont);
  SetTextColor(dc, C::DIMTXT);
  RECT sr = {16, 36, 600, HDR_H};
  DrawTextW(dc, L"Real-time CPU & memory usage by process", -1, &sr,
            DT_LEFT | DT_TOP);

  EndPaint(hWnd, &ps);
}

// ── Window procedure ──────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CREATE: {
    // Fonts
    g_hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_hFontBd = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    RECT rc{};
    GetClientRect(hWnd, &rc);
    int W = rc.right, H = rc.bottom;

    // Search box
    g_hSearch = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, HDR_H + 8,
        260, 24, hWnd, (HMENU)IDC_SEARCH, nullptr, nullptr);
    SendMessage(g_hSearch, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    // Placeholder via EM_SETCUEBANNER
    SendMessage(g_hSearch, 0x1501 /*EM_SETCUEBANNER*/, FALSE,
                (LPARAM)L"Search processes...");

    // Kill button
    g_hKill = CreateWindowExW(
        0, L"BUTTON", L"End Process", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        W - 160, HDR_H + 6, 140, 28, hWnd, (HMENU)IDC_KILL, nullptr, nullptr);
    SendMessage(g_hKill, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // ListView
    int listTop = HDR_H + TOOL_H;
    g_hList = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 0, listTop, W,
        H - listTop - 24, hWnd, (HMENU)IDC_LIST, nullptr, nullptr);
    SetWindowTheme(g_hList, L"Explorer", nullptr);
    SendMessage(g_hList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    ListView_SetExtendedListViewStyle(
        g_hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    ListView_SetBkColor(g_hList, C::BG);
    ListView_SetTextBkColor(g_hList, C::BG);
    ListView_SetTextColor(g_hList, C::TEXT);
    ListView_EnableGroupView(g_hList, TRUE);
    SetupColumns();

    // Status bar
    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0,
                                0, hWnd, (HMENU)IDC_STATUS, nullptr, nullptr);
    SendMessage(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // Cores
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    g_cores = std::max(1, (int)si.dwNumberOfProcessors);

    // Initial collect + timer
    CollectSnapshots();
    RebuildListView();
    SetTimer(hWnd, ID_TIMER, REFRESH_MS, nullptr);
    return 0;
  }
  case WM_TIMER:
    CollectSnapshots();
    RebuildListView();
    return 0;

  case WM_SIZE: {
    int W = LOWORD(lp), H = HIWORD(lp);
    if (g_hList)
      SetWindowPos(g_hList, nullptr, 0, HDR_H + TOOL_H, W,
                   H - HDR_H - TOOL_H - 24, SWP_NOZORDER);
    if (g_hKill)
      SetWindowPos(g_hKill, nullptr, W - 160, HDR_H + 6, 140, 28, SWP_NOZORDER);
    if (g_hStatus)
      SendMessage(g_hStatus, WM_SIZE, 0, 0);
    return 0;
  }
  case WM_PAINT:
    PaintHeader(hWnd);
    return 0;

  case WM_ERASEBKGND: {
    HDC dc = (HDC)wp;
    RECT rc{};
    GetClientRect(hWnd, &rc);
    // Fill toolbar strip
    RECT tr = rc;
    tr.top = HDR_H;
    tr.bottom = HDR_H + TOOL_H;
    HBRUSH tb = CreateSolidBrush(C::PANEL);
    FillRect(dc, &tr, tb);
    DeleteObject(tb);
    return 1;
  }
  case WM_COMMAND:
    if (LOWORD(wp) == IDC_KILL) {
      int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
      if (sel >= 0) {
        wchar_t pidStr[32];
        ListView_GetItemText(g_hList, sel, 1, pidStr, 32);
        DWORD pid = (DWORD)_wtoi(pidStr);
        if (pid > 4) {
          wchar_t msg[128];
          swprintf_s(msg, L"Terminate PID %lu?", pid);
          if (MessageBoxW(hWnd, msg, L"Confirm", MB_YESNO | MB_ICONWARNING) ==
              IDYES) {
            HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hp) {
              TerminateProcess(hp, 1);
              CloseHandle(hp);
            }
            CollectSnapshots();
            RebuildListView();
          }
        }
      }
    }
    if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == IDC_SEARCH) {
      wchar_t buf[128];
      GetWindowTextW(g_hSearch, buf, 128);
      g_filter = buf;
      for (auto &ch : g_filter)
        ch = towlower(ch);
      RebuildListView();
    }
    return 0;

  case WM_NOTIFY: {
    auto *hdr = (LPNMHDR)lp;
    if (hdr->idFrom == IDC_LIST && hdr->code == NM_CUSTOMDRAW)
      return HandleCustomDraw(lp);
    break;
  }
  case WM_CTLCOLOREDIT: {
    HDC dc = (HDC)wp;
    SetBkColor(dc, C::PANEL);
    SetTextColor(dc, C::TEXT);
    static HBRUSH eb = CreateSolidBrush(C::PANEL);
    return (LRESULT)eb;
  }
  case WM_DESTROY:
    KillTimer(hWnd, ID_TIMER);
    DeleteObject(g_hFont);
    DeleteObject(g_hFontBd);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hWnd, msg, wp, lp);
}

// ── Entry point ───────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(C::BG);
  wc.lpszClassName = L"ProcessMonitorClass";
  wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  RegisterClassExW(&wc);

  HWND hWnd = CreateWindowExW(0, L"ProcessMonitorClass", L"Process Monitor",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              900, 650, nullptr, nullptr, hInst, nullptr);

  ShowWindow(hWnd, nShow);
  UpdateWindow(hWnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return (int)msg.wParam;
}
