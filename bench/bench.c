/* bench.c ????015 WinCE ???/?????? v0.1 (Phase 3 ???)
 *
 * ???????Phase 3 ???????????????? *   S1 memcpy ???      ?????????????
 *   S2 BitBlt ??? fps   ????GDI ????????arPlay ?????????
 *   S3 StretchBlt ??? fps ?????????????+?????????
 *   S4 MJPEG ??? fps    ????????????????H.264 ?????????????????
 *   S5 waveOut ???      ????440Hz 3 ?????????????? OK??arPlay ???????? *   S6 ??????          ??????5 ??????????????????/???
 *
 * ?????XE ?????bench_log.txt + ?????????
 * ???????? bench_frame.jpg??00x480 ??? JPEG??? S4?????????
 */

#include <windows.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/* nanojpeg??IT, KeyJ v1.3.5, ??carlive vendor????????*/
#include "nanojpeg.c"

/* ---------------- ??? ---------------- */
static HANDLE g_hLog = INVALID_HANDLE_VALUE;
static DWORD  g_startTick = 0;
static void LogInit(void) {
    WCHAR path[MAX_PATH] = {0};
    g_startTick = GetTickCount();
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        WCHAR *dot = wcsrchr(path, L'.');
        if (dot) *dot = 0;
        wcscat(path, L"_log.txt");
    } else wcscpy(path, L"bench_log.txt");
    g_hLog = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}
static void Log(const char *fmt, ...) {
    if (g_hLog == INVALID_HANDLE_VALUE) return;
    char msg[512];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0; msg[n] = 0;
    char line[600];
    int m = _snprintf(line, sizeof(line) - 1, "[t+%u] %s\r\n",
                      GetTickCount() - g_startTick, msg);
    if (m < 0) m = 0;
    DWORD wr; WriteFile(g_hLog, line, (DWORD)m, &wr, NULL);
    FlushFileBuffers(g_hLog);
}

/* ---------------- wave API ???????????mmsystem.h ?????---------------- */
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec;
                 WORD nBlockAlign, wBitsPerSample, cbSize; } BENCH_WAVEFORMATEX;
typedef struct BENCH_WAVEHDR { LPVOID lpData; DWORD dwBufferLength, dwBytesRecorded;
    DWORD dwUser; DWORD dwFlags, dwLoops; struct BENCH_WAVEHDR *lpNext; DWORD dwReserved; } BENCH_WAVEHDR;
typedef UINT (WINAPI *PFN_waveOutOpen)(LPHANDLE, UINT, BENCH_WAVEFORMATEX*, DWORD, DWORD, DWORD);
typedef UINT (WINAPI *PFN_waveOutPrepareHeader)(HANDLE, BENCH_WAVEHDR*, UINT);
typedef UINT (WINAPI *PFN_waveOutWrite)(HANDLE, BENCH_WAVEHDR*, UINT);
typedef UINT (WINAPI *PFN_waveOutClose)(HANDLE);
#define MMSYSERR_NOERROR 0
#define WHDR_DONE 1

/* ---------------- ??? ---------------- */
static DWORD NowMs(void) { return GetTickCount(); }
/* ??fn ???????????????????????? */

/* ---------------- GUI ????---------------- */
static HWND  g_hwnd = NULL;
static volatile BOOL g_running = TRUE;
static volatile BOOL g_done = FALSE;
static CRITICAL_SECTION g_cs;
static char  g_lines[6][96];      /* ????????6 ?????*/
static volatile int  g_tapCount = 0;
static int   g_taps[8][2];        /* ?????? */

static void SetLine(int i, const char *fmt, ...) {
    if (i < 0 || i > 5) return;
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    _vsnprintf(g_lines[i], 95, fmt, ap);
    va_end(ap);
    LeaveCriticalSection(&g_cs);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

#define WC_BENCH L"BENCH"

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        SHORT x = (SHORT)LOWORD(lp), y = (SHORT)HIWORD(lp);
        if (g_done) { PostQuitMessage(0); return 0; }   /* ????????????=????*/
        if (g_tapCount < 8) {
            g_taps[g_tapCount][0] = x;
            g_taps[g_tapCount][1] = y;
            Log("TAP %d: (%d, %d)", g_tapCount + 1, x, y);
        }
        g_tapCount++;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 255, 0));
        char tmp[6][96];
        EnterCriticalSection(&g_cs);
        memcpy(tmp, g_lines, sizeof(tmp));
        LeaveCriticalSection(&g_cs);
        for (int i = 0; i < 6; i++) {
            WCHAR w[96];
            MultiByteToWideChar(CP_ACP, 0, tmp[i], -1, w, 96);
            ExtTextOutW(hdc, 10, 10 + i * 28, 0, NULL, w, wcslen(w), NULL);
        }
        /* ???????????????????????*/
        HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 0, 0));
        SelectObject(hdc, pen);
        for (int i = 0; i < g_tapCount && i < 8; i++)
            Ellipse(hdc, g_taps[i][0] - 8, g_taps[i][1] - 8,
                    g_taps[i][0] + 8, g_taps[i][1] + 8);
        DeleteObject(pen);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ---------------- ?????---------------- */

static int g_w = 800, g_h = 480;   /* ???????????1/S2 ??*/

static void StageMemcopy(void) {
    SetLine(0, "S1 memcpy...");
    Log("--- S1 memcpy ---");
    DWORD bytes = 800u * 480u * 2u;
    BYTE *src = (BYTE *)LocalAlloc(LMEM_FIXED, bytes);
    BYTE *dst = (BYTE *)LocalAlloc(LMEM_FIXED, bytes);
    if (!src || !dst) { Log("S1 SKIP: alloc failed"); SetLine(0, "S1 SKIP alloc"); return; }
    memset(src, 0xA5, bytes);
    const int N = 60;
    DWORD t0 = NowMs();
    for (int i = 0; i < N && g_running; i++) memcpy(dst, src, bytes);
    DWORD dt = NowMs() - t0;
    if (dt == 0) dt = 1;
    double mbps = (double)N * bytes / 1024.0 / 1024.0 / (dt / 1000.0);
    Log("S1 RESULT: %d x %uKB in %ums = %.1f MB/s", N, bytes / 1024, dt, mbps);
    SetLine(0, "S1 memcpy: %.1f MB/s", mbps);
    LocalFree(src); LocalFree(dst);
}

static void StageBlit(void) {
    Log("--- S2 BitBlt / S3 StretchBlt ---");
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    /* 16bpp 565 DIB??????????? */
    struct { BITMAPINFOHEADER h; DWORD mask[3]; } bi;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = g_w; bi.h.biHeight = g_h;
    bi.h.biPlanes = 1; bi.h.biBitCount = 16;
    bi.h.biCompression = BI_BITFIELDS;
    bi.mask[0] = 0xF800; bi.mask[1] = 0x07E0; bi.mask[2] = 0x001F;
    void *bits = NULL;
    HBITMAP hbmp = CreateDIBSection(hdcScreen, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbmp || !bits) {
        Log("S2 SKIP: CreateDIBSection failed err=%u", GetLastError());
        SetLine(1, "S2 SKIP dib err=%u", GetLastError());
        goto out;
    }
    SelectObject(hdcMem, hbmp);
    /* ?????*/
    {
        unsigned short *p = (unsigned short *)bits;
        for (int y = 0; y < g_h; y++)
            for (int x = 0; x < g_w; x++)
                p[y * g_w + x] =
                    (unsigned short)(((x * 31 / g_w) << 11) | ((y * 63 / g_h) << 5) | 15);
    }
    /* S2: ??? BitBlt */
    SetLine(1, "S2 BitBlt running...");
    {
        const int N = 100;
        DWORD t0 = NowMs();
        for (int i = 0; i < N && g_running; i++)
            BitBlt(hdcScreen, 0, 0, g_w, g_h, hdcMem, 0, 0, SRCCOPY);
        DWORD dt = NowMs() - t0; if (!dt) dt = 1;
        double fps = (double)N * 1000.0 / dt;
        Log("S2 RESULT: BitBlt %dx%d x%d in %ums = %.1f fps", g_w, g_h, N, dt, fps);
        SetLine(1, "S2 BitBlt: %.1f fps", fps);
    }
    /* S3: ????????*/
    SetLine(2, "S3 StretchBlt running...");
    {
        const int N = 60;
        DWORD t0 = NowMs();
        for (int i = 0; i < N && g_running; i++)
            StretchBlt(hdcScreen, 0, 0, g_w, g_h,
                       hdcMem, 0, 0, g_w / 2, g_h / 2, SRCCOPY);
        DWORD dt = NowMs() - t0; if (!dt) dt = 1;
        double fps = (double)N * 1000.0 / dt;
        Log("S3 RESULT: StretchBlt %dx%d->%dx%d x%d in %ums = %.1f fps",
            g_w / 2, g_h / 2, g_w, g_h, N, dt, fps);
        SetLine(2, "S3 Stretch: %.1f fps", fps);
    }
    if (hbmp) DeleteObject(hbmp);
out:
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

static void StageMjpeg(void) {
    Log("--- S4 MJPEG ---");
    SetLine(3, "S4 mjpeg...");
    /* ?????? bench_frame.jpg */
    WCHAR wpath[MAX_PATH] = {0};
    if (GetModuleFileName(NULL, wpath, MAX_PATH)) {
        WCHAR *slash = wcsrchr(wpath, L'\\');
        if (slash) *(slash + 1) = 0;
        wcscat(wpath, L"bench_frame.jpg");
    } else wcscpy(wpath, L"bench_frame.jpg");
    HANDLE hf = CreateFile(wpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        Log("S4 SKIP: bench_frame.jpg not found");
        SetLine(3, "S4 SKIP no jpg");
        return;
    }
    DWORD size = GetFileSize(hf, NULL);
    if (size == 0 || size > 1024u * 1024u) { CloseHandle(hf); Log("S4 SKIP: size %u", size); return; }
    BYTE *buf = (BYTE *)LocalAlloc(LMEM_FIXED, size);
    DWORD rd = 0;
    ReadFile(hf, buf, size, &rd, NULL);
    CloseHandle(hf);
    if (rd != size) { LocalFree(buf); Log("S4 SKIP: read %u/%u", rd, size); return; }

    /* ??????????? */
    njInit();
    if (njDecode(buf, (int)size) != NJ_OK) {
        njDone(); LocalFree(buf);
        Log("S4 FAIL: njDecode error (baseline JPEG only)");
        SetLine(3, "S4 FAIL decode");
        return;
    }
    int jw = njGetWidth(), jh = njGetHeight();
    njDone();
    Log("S4 jpeg %dx%d (%u bytes)", jw, jh, size);

    const int N = 30;
    DWORD t0 = NowMs();
    for (int i = 0; i < N && g_running; i++) {
        njInit();
        njDecode(buf, (int)size);
        njDone();
    }
    DWORD dt = NowMs() - t0; if (!dt) dt = 1;
    double fps = (double)N * 1000.0 / dt;
    Log("S4 RESULT: MJPEG %dx%d x%d in %ums = %.1f fps", jw, jh, N, dt, fps);
    SetLine(3, "S4 mjpeg: %.1f fps", fps);
    LocalFree(buf);
}

static void StageAudio(void) {
    Log("--- S5 waveOut ---");
    SetLine(4, "S5 tone...");
    HMODULE mm = GetModuleHandle(L"coredll.dll");
    if (!mm) mm = LoadLibrary(L"coredll.dll");
    if (!mm) { Log("S5 SKIP: no coredll"); SetLine(4, "S5 SKIP"); return; }
    PFN_waveOutOpen pOpen = (PFN_waveOutOpen)GetProcAddress(mm, L"waveOutOpen");
    PFN_waveOutPrepareHeader pPrep = (PFN_waveOutPrepareHeader)GetProcAddress(mm, L"waveOutPrepareHeader");
    PFN_waveOutWrite pWrite = (PFN_waveOutWrite)GetProcAddress(mm, L"waveOutWrite");
    PFN_waveOutClose pClose = (PFN_waveOutClose)GetProcAddress(mm, L"waveOutClose");
    if (!pOpen || !pPrep || !pWrite || !pClose) {
        Log("S5 SKIP: waveOut API missing"); SetLine(4, "S5 SKIP api"); return;
    }
    const int RATE = 22050, SECS = 3;
    DWORD nSamples = (DWORD)RATE * SECS;
    DWORD nBytes = nSamples * 2;
    short *pcm = (short *)LocalAlloc(LMEM_FIXED, nBytes);
    if (!pcm) { Log("S5 SKIP: alloc"); return; }
    /* ????????256 ???440Hz */
    static short sineTab[256];
    static BOOL tabInit = FALSE;
    if (!tabInit) {
        for (int i = 0; i < 256; i++) {
            double a = 6.283185307179586 * i / 256.0;
            double a2 = a;
            while (a2 > 3.14159265) a2 -= 6.28318531;
            double x = a2;
            double s = x * (1.0 - x * x / 6.0 * (1.0 - x * x / 20.0 * (1.0 - x * x / 42.0)));
            sineTab[i] = (short)(s * 9000.0);
        }
        tabInit = TRUE;
    }
    for (DWORD i = 0; i < nSamples; i++)
        pcm[i] = sineTab[(i * 440u * 256u / RATE) & 255];   /* 440Hz ?????? */

    BENCH_WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = 1 /*WAVE_FORMAT_PCM*/;
    wf.nChannels = 1;
    wf.nSamplesPerSec = RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = RATE * 2;

    HANDLE hWave = NULL;
    UINT rc = pOpen(&hWave, (UINT)-1 /*WAVE_MAPPER*/, &wf, 0, 0, 0 /*CALLBACK_NULL*/);
    Log("S5 waveOutOpen rc=%u (%s)", rc,
        rc == MMSYSERR_NOERROR ? "OK" : "FAILED");
    if (rc != MMSYSERR_NOERROR || !hWave) {
        SetLine(4, "S5 audio FAILED rc=%u", rc);
        LocalFree(pcm);
        return;
    }
    BENCH_WAVEHDR hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.lpData = pcm;
    hdr.dwBufferLength = nBytes;
    pPrep(hWave, &hdr, sizeof(hdr));
    UINT rcw = pWrite(hWave, &hdr, sizeof(hdr));
    Log("S5 waveOutWrite rc=%u, playing 3s (listen!)", rcw);
    SetLine(4, "S5 tone PLAYING - listen");
    DWORD t0 = NowMs();
    while (NowMs() - t0 < 3500 && !(hdr.dwFlags & WHDR_DONE)) Sleep(100);
    pClose(hWave);
    Log("S5 done. HEARD tone? -> audio path %s",
        rcw == MMSYSERR_NOERROR ? "likely OK (?????????)" : "write failed");
    SetLine(4, "S5 audio rc=%u (hear it?)", rcw);
    LocalFree(pcm);
}

static void StageTouch(void) {
    Log("--- S6 touch: tap 5 points (4 corners + center) ---");
    SetLine(5, "S6 tap 5 points!");
    g_tapCount = 0;
    DWORD t0 = NowMs();
    while (g_tapCount < 5 && NowMs() - t0 < 40000 && g_running) Sleep(200);
    Log("S6 RESULT: %d taps collected", g_tapCount);
    SetLine(5, "S6 taps: %d (see log/circles)", g_tapCount);
}

/* ---------------- worker ---------------- */
static DWORD WINAPI BenchThread(LPVOID arg) {
    (void)arg;
    Sleep(500);   /* ????????*/
    Log("==== bench v0.1 start screen=%dx%d ====", g_w, g_h);
    StageMemcopy();
    StageBlit();
    StageMjpeg();
    StageAudio();
    StageTouch();
    Log("==== bench done %u ms ??tap screen to exit ====", GetTickCount() - g_startTick);
    SetLine(0, "DONE - tap to exit");
    if (g_hLog != INVALID_HANDLE_VALUE) CloseHandle(g_hLog);
    g_hLog = INVALID_HANDLE_VALUE;
    g_done = TRUE;
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int nShow) {
    (void)hPrev; (void)cmd;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WC_BENCH;
    RegisterClassW(&wc);

    g_w = GetSystemMetrics(SM_CXSCREEN);
    g_h = GetSystemMetrics(SM_CYSCREEN);

    InitializeCriticalSection(&g_cs);
    LogInit();
    strcpy(g_lines[0], "bench starting...");

    g_hwnd = CreateWindowW(WC_BENCH, L"bench", WS_VISIBLE | WS_CAPTION,
                           0, 0, g_w, g_h, NULL, NULL, hInst, NULL);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    HANDLE th = CreateThread(NULL, 256*1024, BenchThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    g_running = FALSE;   /* ??? worker ???????????*/
    if (th) { WaitForSingleObject(th, 45000); CloseHandle(th); }
    if (g_hLog != INVALID_HANDLE_VALUE) CloseHandle(g_hLog);
    return 0;
}
