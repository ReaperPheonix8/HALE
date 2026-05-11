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

#include <windows.h>
#include <algorithm>
#include <commctrl.h>
#include <cwctype>
#include <iomanip>
#include <map>
#include <psapi.h>
#include <shellapi.h>
#include <sstream>
#include <string>
#include <tlhelp32.h>
#include <unordered_map>
#include <unordered_set>
#include <uxtheme.h>
#include <vector>
#include <deque>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

// ── IDs ──────────────────────────────────────────────────────────
static const int ID_TIMER = 1;
static const int IDC_LIST = 101;
static const int IDC_STATUS = 102;
static const int IDC_SEARCH = 103;
static const int IDC_REFRESH = 104;
static const int IDC_UNIT = 105;
static const int REFRESH_MS = 500;
static const int HDR_H = 64;  // header banner height (px)
static const int TOOL_H = 40; // toolbar strip height (px)

// ── Palette ───────────────────────────────────────────────────────
namespace C {
COLORREF BG = RGB(10, 10, 20);        // Deeper Space Black
COLORREF PANEL = RGB(20, 20, 40);     // Nebula Purple
COLORREF ACCENT = RGB(0, 255, 255);   // Cyber Cyan
COLORREF TEXT = RGB(220, 230, 255);   // Starlight Blue
COLORREF DIMTXT = RGB(120, 130, 180); // Comet Blue
COLORREF ROWALT = RGB(15, 15, 30);    // Alternate row
COLORREF SEL = RGB(50, 50, 150);      // Selection Glow
COLORREF GRPBG = RGB(30, 30, 60);     // Group Header
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
  SIZE_T totalPeakRam = 0;
  SIZE_T totalPriv = 0;
};

// ── Globals ───────────────────────────────────────────────────────
static HWND g_hWnd = nullptr;
static HWND g_hList = nullptr;
static HWND g_hStatus = nullptr;
static HWND g_hSearch = nullptr;
static HWND g_hRefresh = nullptr;
static HFONT g_hFont = nullptr;
static HFONT g_hFontBd = nullptr;
static int g_cores = 1;
static std::unordered_map<DWORD, ProcessInfo> g_prev; // CPU delta store
static std::vector<ProcessGroup> g_groups;
static std::wstring g_filter; // live search filter

static std::unordered_set<std::wstring> g_expandedGroups;
enum class MemUnit { Auto, MB, GB };
static MemUnit g_unit = MemUnit::Auto;
static HWND g_hUnit = nullptr;

static HIMAGELIST g_hIml = nullptr;
static std::unordered_map<std::wstring, int> g_iconCache;
static SIZE_T g_totalSysRam = 1;

static std::deque<double> g_cpuHistory;
static std::deque<double> g_ramHistory;
static const int MAX_HISTORY = 120;
static const int SIDE_W = 320; // Side panel width

static ULONGLONG g_lastSysIdle = 0, g_lastSysKern = 0, g_lastSysUser = 0;

// ── Helpers ───────────────────────────────────────────────────────

std::wstring FormatBytes(SIZE_T b) {
  wchar_t buf[64];
  if (g_unit == MemUnit::GB ||
      (g_unit == MemUnit::Auto && b >= (SIZE_T)1 << 30))
    swprintf_s(buf, L"%.2f GB", b / 1073741824.0);
  else if (g_unit == MemUnit::MB ||
           (g_unit == MemUnit::Auto && b >= (SIZE_T)1 << 20))
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

        // Icon Cache
        if (g_iconCache.find(info.name) == g_iconCache.end()) {
          wchar_t path[MAX_PATH];
          DWORD size = MAX_PATH;
          if (QueryFullProcessImageNameW(hp, 0, path, &size)) {
            HICON hIcon[1] = {0};
            ExtractIconExW(path, 0, nullptr, hIcon, 1);
            if (hIcon[0]) {
              int imgIdx = ImageList_AddIcon(g_hIml, hIcon[0]);
              g_iconCache[info.name] = imgIdx;
              DestroyIcon(hIcon[0]);
            } else {
              g_iconCache[info.name] = -1;
            }
          } else {
            g_iconCache[info.name] = -1;
          }
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
    g.totalPeakRam = 0;
    g.totalPriv = 0;
    for (auto &p : g.members) {
      g.totalCpu += p.cpuPct;
      g.totalRam += p.ramBytes;
      g.totalPeakRam = std::max(g.totalPeakRam, p.peakRamBytes);
      g.totalPriv += p.privBytes;
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

  // High-Accuracy System CPU for Dashboard
  FILETIME idle, kern, usr;
  if (GetSystemTimes(&idle, &kern, &usr)) {
      ULONGLONG i = FT2ULL(idle), k = FT2ULL(kern), u = FT2ULL(usr);
      if (g_lastSysIdle > 0) {
          ULONGLONG dIdle = i - g_lastSysIdle;
          ULONGLONG dKern = k - g_lastSysKern;
          ULONGLONG dUsr = u - g_lastSysUser;
          ULONGLONG total = dKern + dUsr;
          if (total > 0) {
              double sysCpu = (double)(total - dIdle) / total * 100.0;
              g_cpuHistory.push_back(std::max(0.0, std::min(100.0, sysCpu)));
          }
      }
      g_lastSysIdle = i; g_lastSysKern = k; g_lastSysUser = u;
  }

  // System RAM History
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (GlobalMemoryStatusEx(&mem)) {
      g_ramHistory.push_back((double)mem.dwMemoryLoad);
  }

  if (g_cpuHistory.size() > MAX_HISTORY) g_cpuHistory.pop_front();
  if (g_ramHistory.size() > MAX_HISTORY) g_ramHistory.pop_front();
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
  ListView_DeleteAllItems(g_hList);

  int idx = 0;
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

    bool expanded = g_expandedGroups.count(grp.name) > 0;

    // Parent row
    wchar_t hdr[300], cpu[32], ram[32], peak[32], priv[32];
    swprintf_s(hdr, L"%s %s (%zu)", expanded ? L"[-]" : L"[+]",
               grp.name.c_str(), grp.members.size());
    swprintf_s(cpu, L"%.1f%%", grp.totalCpu);
    wcscpy_s(ram, FormatBytes(grp.totalRam).c_str());
    wcscpy_s(peak, FormatBytes(grp.totalPeakRam).c_str());
    wcscpy_s(priv, FormatBytes(grp.totalPriv).c_str());

    int iconIdx = -1;
    if (g_iconCache.count(grp.name) > 0)
      iconIdx = g_iconCache[grp.name];

    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM | (iconIdx >= 0 ? LVIF_IMAGE : 0);
    item.iItem = idx;
    item.iSubItem = 0;
    item.pszText = hdr;
    item.lParam = 1; // 1 = Group Header
    if (iconIdx >= 0)
      item.iImage = iconIdx;
    ListView_InsertItem(g_hList, &item);
    ListView_SetItemText(g_hList, idx, 1, (LPWSTR)L"");
    ListView_SetItemText(g_hList, idx, 2, cpu);
    ListView_SetItemText(g_hList, idx, 3, ram);
    ListView_SetItemText(g_hList, idx, 4, peak);
    ListView_SetItemText(g_hList, idx, 5, priv);
    ++idx;

    if (expanded) {
      for (auto &p : grp.members) {
        wchar_t pcpu[32], pram[32], ppeak[32], ppriv[32], ppid[16], pname[300];
        swprintf_s(pname, L"    %s", p.name.c_str());
        swprintf_s(pcpu, L"%.1f%%", p.cpuPct);
        swprintf_s(ppid, L"%lu", p.pid);
        wcscpy_s(pram, FormatBytes(p.ramBytes).c_str());
        wcscpy_s(ppeak, FormatBytes(p.peakRamBytes).c_str());
        wcscpy_s(ppriv, FormatBytes(p.privBytes).c_str());

        LVITEMW citem{};
        citem.mask = LVIF_TEXT | LVIF_PARAM;
        citem.iItem = idx;
        citem.iSubItem = 0;
        citem.pszText = pname;
        citem.lParam = 0; // 0 = Child
        ListView_InsertItem(g_hList, &citem);
        ListView_SetItemText(g_hList, idx, 1, ppid);
        ListView_SetItemText(g_hList, idx, 2, pcpu);
        ListView_SetItemText(g_hList, idx, 3, pram);
        ListView_SetItemText(g_hList, idx, 4, ppeak);
        ListView_SetItemText(g_hList, idx, 5, ppriv);
        ++idx;
      }
    }
  }

  SendMessage(g_hList, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g_hList, nullptr, FALSE);

  // Status bar
  wchar_t sb[200];
  swprintf_s(sb,
             L"  %d list rows   |   Total RAM: %s   |   Total CPU: %.1f%%   |  "
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
    LVITEMW item{};
    item.iItem = row;
    item.mask = LVIF_PARAM;
    ListView_GetItem(g_hList, &item);

    bool isGroup = (item.lParam == 1);
    bool isHot = (ListView_GetHotItem(g_hList) == row);

    COLORREF bg = isGroup ? C::GRPBG : ((row % 2 == 0) ? C::BG : C::ROWALT);

    UINT state = ListView_GetItemState(g_hList, row, LVIS_SELECTED);
    if (state & LVIS_SELECTED)
      bg = C::SEL;
    else if (isHot && !isGroup)
      bg = RGB(45, 50, 55); // hover color

    cd->clrTextBk = bg;
    cd->clrText = isGroup ? RGB(255, 255, 255) : C::TEXT;

    SelectObject(cd->nmcd.hdc, isGroup ? g_hFontBd : g_hFont);

    return CDRF_NOTIFYSUBITEMDRAW | CDRF_NEWFONT;
  }
  case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
    int row = (int)cd->nmcd.dwItemSpec;
    LVITEMW item{};
    item.iItem = row;
    item.mask = LVIF_PARAM;
    ListView_GetItem(g_hList, &item);

    bool isGroup = (item.lParam == 1);
    bool isHot = (ListView_GetHotItem(g_hList) == row);

    UINT state = ListView_GetItemState(g_hList, row, LVIS_SELECTED);
    COLORREF bg = isGroup ? C::GRPBG : ((row % 2 == 0) ? C::BG : C::ROWALT);
    if (state & LVIS_SELECTED)
      bg = C::SEL;
    else if (isHot && !isGroup)
      bg = RGB(45, 50, 55);

    cd->clrTextBk = bg;

    if (cd->iSubItem == 2 || cd->iSubItem == 3) {
      RECT rc;
      ListView_GetSubItemRect(g_hList, row, cd->iSubItem, LVIR_BOUNDS, &rc);

      HBRUSH bgBr = CreateSolidBrush(bg);
      FillRect(cd->nmcd.hdc, &rc, bgBr);
      DeleteObject(bgBr);

      wchar_t buf[64];
      ListView_GetItemText(g_hList, row, cd->iSubItem, buf, 64);

      double ratio = 0.0;
      COLORREF barCol = 0;
      if (cd->iSubItem == 2) {
        double v = _wtof(buf);
        ratio = std::min(1.0, v / (g_cores * 100.0));
        barCol = RGB(v < 5 ? 40 : v < 20 ? 80 : 180, v < 20 ? 120 : 60, 40);
      } else {
        double val = _wtof(buf);
        if (wcsstr(buf, L"GB"))
          val *= 1073741824.0;
        else if (wcsstr(buf, L"MB"))
          val *= 1048576.0;
        else if (wcsstr(buf, L"KB"))
          val *= 1024.0;
      ratio = std::min(1.0, val / (double)std::max((SIZE_T)1, g_totalSysRam));
        barCol = RGB(100, 50, 255); // Vibrant Nebula Purple
      }

      RECT barRc = rc;
      barRc.right = barRc.left + (int)((barRc.right - barRc.left) * ratio);
      if (barRc.right > barRc.left) {
        HBRUSH br = CreateSolidBrush(barCol);
        FillRect(cd->nmcd.hdc, &barRc, br);
        DeleteObject(br);
        
        // Neon Glow Border
        HPEN glowPen = CreatePen(PS_SOLID, 1, barCol);
        HGDIOBJ oldPen = SelectObject(cd->nmcd.hdc, glowPen);
        MoveToEx(cd->nmcd.hdc, barRc.left, barRc.top, nullptr);
        LineTo(cd->nmcd.hdc, barRc.right, barRc.top);
        MoveToEx(cd->nmcd.hdc, barRc.left, barRc.bottom - 1, nullptr);
        LineTo(cd->nmcd.hdc, barRc.right, barRc.bottom - 1);
        SelectObject(cd->nmcd.hdc, oldPen);
        DeleteObject(glowPen);
      }

      SetBkMode(cd->nmcd.hdc, TRANSPARENT);
      SetTextColor(cd->nmcd.hdc, RGB(255, 255, 255));
      SelectObject(cd->nmcd.hdc, isGroup ? g_hFontBd : g_hFont);
      rc.right -= 6;
      DrawTextW(cd->nmcd.hdc, buf, -1, &rc,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

      return CDRF_SKIPDEFAULT;
    }

    if (isGroup) {
      cd->clrText = RGB(255, 255, 255);
    } else {
      cd->clrText = C::TEXT;
    }

    SelectObject(cd->nmcd.hdc, isGroup ? g_hFontBd : g_hFont);
    return CDRF_NEWFONT;
  }
  }
  return CDRF_DODEFAULT;
}

// ── Header banner paint ───────────────────────────────────────────
void PaintHeader(HWND hWnd, HDC dc) {
  RECT rc{};
  GetClientRect(hWnd, &rc);
  rc.bottom = HDR_H;
  // gradient: Deep Cosmic Gradient
  for (int x = rc.left; x < rc.right; ++x) {
    double t = (double)x / std::max(1L, rc.right - rc.left);
    COLORREF col =
        RGB((BYTE)(10 + t * 40), (BYTE)(10 + t * 15), (BYTE)(30 + t * 80));
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
}

void DrawDashboard(HWND hWnd, HDC dc) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    
    int dashX = rc.right - SIDE_W;
    if (dashX < 0) return;
    
    RECT dr = { dashX, HDR_H + TOOL_H, rc.right, rc.bottom - 24 };
    HBRUSH dashBr = CreateSolidBrush(C::PANEL);
    FillRect(dc, &dr, dashBr);
    DeleteObject(dashBr);

    // Separator line
    HPEN sep = CreatePen(PS_SOLID, 1, RGB(40, 40, 80));
    SelectObject(dc, sep);
    MoveToEx(dc, dashX, dr.top, nullptr);
    LineTo(dc, dashX, dr.bottom);
    DeleteObject(sep);

    auto DrawGraph = [&](const std::deque<double>& data, int y, const wchar_t* title, COLORREF col) {
        SetTextColor(dc, C::ACCENT);
        SelectObject(dc, g_hFontBd);
        RECT tr = { dashX + 20, y, rc.right - 20, y + 30 };
        DrawTextW(dc, title, -1, &tr, DT_LEFT | DT_TOP);
        
        RECT gr = { dashX + 20, y + 35, rc.right - 20, y + 135 };
        HBRUSH grBr = CreateSolidBrush(C::BG);
        FillRect(dc, &gr, grBr);
        DeleteObject(grBr);

        if (data.size() < 2) return;
        HPEN graphPen = CreatePen(PS_SOLID, 2, col);
        SelectObject(dc, graphPen);
        
        double step = (double)(gr.right - gr.left) / (MAX_HISTORY - 1);
        int startIdx = MAX_HISTORY - (int)data.size();
        
        for (size_t i = 0; i < data.size(); ++i) {
            int x = gr.left + (int)((startIdx + i) * step);
            int py = gr.bottom - (int)(data[i] * (gr.bottom - gr.top) / 100.0);
            if (i == 0) MoveToEx(dc, x, py, nullptr);
            else LineTo(dc, x, py);
        }
        DeleteObject(graphPen);

        // Value text
        wchar_t val[32];
        swprintf_s(val, L"%.1f%%", data.back());
        SetTextColor(dc, C::TEXT);
        SelectObject(dc, g_hFont);
        RECT vr = { gr.right - 60, gr.top - 20, gr.right, gr.top };
        DrawTextW(dc, val, -1, &vr, DT_RIGHT | DT_TOP);
    };

    DrawGraph(g_cpuHistory, dr.top + 160, L"System CPU Usage", RGB(0, 255, 255));
    DrawGraph(g_ramHistory, dr.top + 330, L"System RAM Usage", RGB(200, 100, 255));

    // System Info Header
    SetTextColor(dc, RGB(255, 255, 255));
    SelectObject(dc, g_hFontBd);
    RECT ir = { dashX + 20, dr.top + 10, rc.right - 20, dr.top + 40 };
    DrawTextW(dc, L"System Overview", -1, &ir, DT_LEFT | DT_TOP);
    
    SelectObject(dc, g_hFont);
    SetTextColor(dc, C::DIMTXT);
    wchar_t info[256];
    SYSTEM_INFO si; GetSystemInfo(&si);
    swprintf_s(info, L"Cores: %u | Threads: %u\nUptime: %llu min", 
               g_cores, (unsigned)g_groups.size(), GetTickCount64() / 60000);
    RECT irr = { dashX + 20, dr.top + 45, rc.right - 20, dr.top + 140 };
    DrawTextW(dc, info, -1, &irr, DT_LEFT | DT_TOP);

    // Top Eaters
    int ty = dr.top + 500;
    SetTextColor(dc, C::ACCENT);
    SelectObject(dc, g_hFontBd);
    RECT trr = { dashX + 20, ty, rc.right - 20, ty + 30 };
    DrawTextW(dc, L"Top Resource Eaters", -1, &trr, DT_LEFT | DT_TOP);

    std::vector<ProcessGroup*> sorted = {};
    for (auto& g : g_groups) sorted.push_back(&g);
    std::sort(sorted.begin(), sorted.end(), [](ProcessGroup* a, ProcessGroup* b) {
        return a->totalRam > b->totalRam;
    });

    for (int i = 0; i < std::min(5, (int)sorted.size()); ++i) {
        SetTextColor(dc, C::TEXT);
        SelectObject(dc, g_hFont);
        wchar_t item[128];
        swprintf_s(item, L"%d. %s (%s)", i+1, sorted[i]->name.c_str(), FormatBytes(sorted[i]->totalRam).c_str());
        RECT r = { dashX + 25, ty + 40 + (i * 25), rc.right - 20, ty + 65 + (i * 25) };
        DrawTextW(dc, item, -1, &r, DT_LEFT | DT_TOP | DT_END_ELLIPSIS);
    }
}

// ── Window procedure ──────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CREATE: {
    // Initialize System Stats
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_cores = si.dwNumberOfProcessors;

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    g_totalSysRam = (SIZE_T)ms.ullTotalPhys;

    // Modern Fonts (Segoe UI Variable if available)
    g_hFont = CreateFontW(15, 0, 0, 0, FW_LIGHT, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Display");
    if (!g_hFont) g_hFont = CreateFontW(15, 0, 0, 0, FW_LIGHT, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                          
    g_hFontBd = CreateFontW(20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    if (!g_hFontBd) g_hFontBd = CreateFontW(20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Semibold");

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

    // Refresh button
    g_hRefresh = CreateWindowExW(
        0, L"BUTTON", L"Refresh Now", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        W - 160, HDR_H + 6, 140, 28, hWnd, (HMENU)IDC_REFRESH, nullptr, nullptr);
    SendMessage(g_hRefresh, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // Unit button
    g_hUnit = CreateWindowExW(
        0, L"BUTTON", L"Unit Toggle", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        W - 310, HDR_H + 6, 140, 28, hWnd, (HMENU)IDC_UNIT, nullptr, nullptr);
    SendMessage(g_hUnit, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // ListView
    int listTop = HDR_H + TOOL_H;
    g_hList = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 0, listTop, W,
        H - listTop - 24, hWnd, (HMENU)IDC_LIST, nullptr, nullptr);
    SetWindowTheme(g_hList, L"Explorer", nullptr);
    SendMessage(g_hList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT |
                                                   LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(g_hList, C::BG);
    ListView_SetTextBkColor(g_hList, C::BG);
    ListView_SetTextColor(g_hList, C::TEXT);
    g_hIml = ImageList_Create(24, 24, ILC_COLOR32 | ILC_MASK, 1, 1);
    ListView_SetImageList(g_hList, g_hIml, LVSIL_SMALL);
    SetupColumns();

    // Status bar
    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0,
                                0, hWnd, (HMENU)IDC_STATUS, nullptr, nullptr);
    SendMessage(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);



    // Initial collect + timer
    CollectSnapshots();
    RebuildListView();
    SetTimer(hWnd, ID_TIMER, REFRESH_MS, nullptr);
    return 0;
  }
  case WM_TIMER: {
    CollectSnapshots();
    RebuildListView();
    
    // Efficiency: Only invalidate the dashboard area on the right
    RECT rc;
    GetClientRect(hWnd, &rc);
    RECT dashRc = { rc.right - SIDE_W, HDR_H + TOOL_H, rc.right, rc.bottom };
    InvalidateRect(hWnd, &dashRc, FALSE);
    return 0;
  }

  case WM_SIZE: {
    int W = LOWORD(lp), H = HIWORD(lp);
    int listW = (W > SIDE_W + 200) ? W - SIDE_W : W;
    
    if (g_hList) {
      SetWindowPos(g_hList, nullptr, 0, HDR_H + TOOL_H, listW,
                   H - HDR_H - TOOL_H - 24, SWP_NOZORDER);
      
      // Auto-stretch the first column (Process Name)
      int totalColW = 0;
      for (int i = 1; i < 6; ++i) totalColW += ListView_GetColumnWidth(g_hList, i);
      ListView_SetColumnWidth(g_hList, 0, std::max(200, listW - totalColW - 20));
    }
    if (g_hRefresh)
      SetWindowPos(g_hRefresh, nullptr, W - 160, HDR_H + 6, 140, 28, SWP_NOZORDER);
    if (g_hUnit)
      SetWindowPos(g_hUnit, nullptr, W - 310, HDR_H + 6, 140, 28, SWP_NOZORDER);
    if (g_hStatus)
      SendMessage(g_hStatus, WM_SIZE, 0, 0);
    return 0;
  }
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT rc; GetClientRect(hWnd, &rc);
    
    // Double buffering for entire window
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBM = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    SelectObject(memDC, memBM);
    
    // Fill background
    HBRUSH bgBr = CreateSolidBrush(C::BG);
    FillRect(memDC, &rc, bgBr);
    DeleteObject(bgBr);
    
    PaintHeader(hWnd, memDC);
    DrawDashboard(hWnd, memDC);
    
    // Fill toolbar strip area
    RECT tr = rc;
    tr.top = HDR_H;
    tr.bottom = HDR_H + TOOL_H;
    HBRUSH tb = CreateSolidBrush(C::PANEL);
    FillRect(memDC, &tr, tb);
    DeleteObject(tb);
    
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
    
    DeleteObject(memBM);
    DeleteDC(memDC);
    EndPaint(hWnd, &ps);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1; // Prevent flickering by not erasing background

  case WM_DRAWITEM: {
    auto *dis = (LPDRAWITEMSTRUCT)lp;
    if (dis->CtlID == IDC_REFRESH || dis->CtlID == IDC_UNIT) {
      bool selected = (dis->itemState & ODS_SELECTED);
      bool disabled = (dis->itemState & ODS_DISABLED);

      COLORREF bg = C::PANEL;
      if (selected)
        bg = RGB(80, 40, 180); // Electric Purple
      else if (!disabled)
        bg = RGB(45, 45, 85); // Deep Nebula

      HBRUSH br = CreateSolidBrush(bg);
      FillRect(dis->hDC, &dis->rcItem, br);
      DeleteObject(br);

      HPEN border = CreatePen(PS_SOLID, 1, C::ACCENT);
      HGDIOBJ oldPen = SelectObject(dis->hDC, border);
      HGDIOBJ oldBr = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
      Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right,
                dis->rcItem.bottom);
      SelectObject(dis->hDC, oldPen);
      SelectObject(dis->hDC, oldBr);
      DeleteObject(border);

      wchar_t text[64];
      GetWindowTextW(dis->hwndItem, text, 64);

      SetBkMode(dis->hDC, TRANSPARENT);
      SetTextColor(dis->hDC,
                   disabled ? RGB(100, 100, 130) : RGB(255, 255, 255));
      SelectObject(dis->hDC, g_hFont);
      DrawTextW(dis->hDC, text, -1, &dis->rcItem,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      return TRUE;
    }
    return 0;
  }

  case WM_COMMAND:
    if (LOWORD(wp) == IDC_REFRESH) {
      CollectSnapshots();
      RebuildListView();
    }
    if (LOWORD(wp) == IDC_UNIT) {
      if (g_unit == MemUnit::Auto) {
        g_unit = MemUnit::MB;
        SetWindowTextW(g_hUnit, L"Unit: MB");
      } else if (g_unit == MemUnit::MB) {
        g_unit = MemUnit::GB;
        SetWindowTextW(g_hUnit, L"Unit: GB");
      } else {
        g_unit = MemUnit::Auto;
        SetWindowTextW(g_hUnit, L"Unit: Auto");
      }
      RebuildListView();
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
    if (hdr->idFrom == IDC_LIST && hdr->code == NM_DBLCLK) {
      auto *nmv = (LPNMITEMACTIVATE)lp;
      if (nmv->iItem >= 0) {
        LVITEMW item{};
        item.iItem = nmv->iItem;
        item.mask = LVIF_PARAM;
        if (ListView_GetItem(g_hList, &item) && item.lParam == 1) {
          wchar_t buf[300];
          ListView_GetItemText(g_hList, nmv->iItem, 0, buf, 300);
          std::wstring text = buf;
          size_t start = text.find(L" ");
          size_t end = text.rfind(L" (");
          if (start != std::wstring::npos && end != std::wstring::npos &&
              start < end) {
            std::wstring name = text.substr(start + 1, end - start - 1);
            if (g_expandedGroups.count(name))
              g_expandedGroups.erase(name);
            else
              g_expandedGroups.insert(name);
            RebuildListView();
          }
        }
      }
    }
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
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
  wc.lpszClassName = L"ProcessMonitorClass";
  wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  RegisterClassExW(&wc);

  HWND hWnd = CreateWindowExW(0, L"ProcessMonitorClass", L"Process Monitor",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                              CW_USEDEFAULT, CW_USEDEFAULT, 900, 650, nullptr, nullptr, hInst, nullptr);

  ShowWindow(hWnd, nShow);
  UpdateWindow(hWnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return (int)msg.wParam;
}
