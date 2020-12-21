extern "C" {
/* clang-format off */
#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
/* clang-format on */
}

#include "vid_receiver_common.h"
#include <cstdarg>

#define IDI_NETWORK_TELEVISION 101
#define IDI_CAMERA 102

#define WAITING_WIDTH 432
#define WAITING_HEIGHT 240

static HINSTANCE AppInstance = nullptr;
static HICON SnapIcon = nullptr;
static HBITMAP VideoBMP = nullptr;
static unsigned char *VideoBMPBits = nullptr;
static HDC VideoMemDc = nullptr;
static HGDIOBJ OldVideoMemDcObj = nullptr;
static DWORD MainThreadId = 0;
static CRITICAL_SECTION PaintCritSec;
static HWND Hwnd = nullptr;
static HWND SnapshotButton = nullptr;
static HWND QualityTrackbar = nullptr;
static int ReqWidth = 0, ReqHeight = 0;
static int WindowWidth = 0, WindowHeight = 0;
static int BMPWidth = 0, BMPHeight = 0;
static int Bitrate = 0;
static const char *Title = nullptr;
static const char *Comment = nullptr;
static sockaddr_in SdpAddr = {};
static int SdpPort = 0;
bool NoQuit = false;

static volatile bool g_running = true;

static void MessageErrorToUser(const char *text, ...) {
  char msg_buf[256];
  va_list va;
  va_start(va, text);
  vsnprintf(msg_buf, 256, text, va);
  va_end(va);

  wchar_t wmsg_buf[256];
  MultiByteToWideChar(CP_ACP, 0, msg_buf, -1, wmsg_buf, 256);
  MessageBox(nullptr, wmsg_buf, TEXT("ffmpeg error"), MB_OK | MB_ICONERROR);
}

static void GetAddrInfo(DisplayContext &ctx) {
  struct sockaddr_storage addr = ctx.sdp_ip(SdpPort);
  memcpy(&SdpAddr, &addr, sizeof(SdpAddr));
}

static DWORD WINAPI VideoTask(LPVOID firstArg) {
  // av_log_set_level(AV_LOG_VERBOSE);

  char sdp_path[MAX_PATH];
  WideCharToMultiByte(CP_ACP, 0, LPTSTR(firstArg), -1, sdp_path, MAX_PATH,
                      nullptr, nullptr);
  printf("Opening %s\n", sdp_path);

  DisplayContext ctx(sdp_path, ControlInterface::make_addr(CTRL_NET_PATH));
  if (!ctx.init(MessageErrorToUser))
    abort();

  Title = ctx.title();
  Comment = ctx.comment();
  GetAddrInfo(ctx);
  PostThreadMessage(MainThreadId, WM_USER + 2, 0, 0);

  while (g_running)
    ctx.do_frame();

  return 0;
}

void DisplayVideoFrame(SwsContext *sws_ctx, AVFrame *net_frame, int bitrate) {
  EnterCriticalSection(&PaintCritSec);

  ReqWidth = net_frame->width;
  ReqHeight = net_frame->height;
  Bitrate = bitrate;

  if (VideoMemDc && net_frame->width == BMPWidth &&
      net_frame->height == BMPHeight) {
    int linesize = WindowWidth * 4;
    sws_scale(sws_ctx, (const uint8_t *const *)net_frame->data,
              net_frame->linesize, 0, net_frame->height, &VideoBMPBits,
              &linesize);
  }

  LeaveCriticalSection(&PaintCritSec);
  PostThreadMessage(MainThreadId, WM_USER, 0, 0);
}

static int SnapReqWidth = 0, SnapReqHeight = 0;
static LPVOID SnapBits = nullptr;
static bool SnapPending = false;

void DisplaySnapshotFrame(SwsContext *sws_ctx, AVFrame *net_frame) {
  EnterCriticalSection(&PaintCritSec);

  if (!SnapBits || SnapReqWidth != net_frame->width ||
      SnapReqHeight != net_frame->height) {
    if (SnapBits)
      free(SnapBits);

    SnapReqWidth = net_frame->width;
    SnapReqHeight = net_frame->height;

    SnapBits = malloc(net_frame->width * net_frame->height * 4);
  }

  int linesize = net_frame->width * 4;
  sws_scale(sws_ctx, (const uint8_t *const *)net_frame->data,
            net_frame->linesize, 0, net_frame->height,
            (uint8_t *const *)&SnapBits, &linesize);

  SnapPending = true;
  LeaveCriticalSection(&PaintCritSec);
  PostThreadMessage(MainThreadId, WM_USER + 1, 0, 0);
}

static void CreateControls(HWND parent);

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                                   LPARAM lParam) {
  switch (uMsg) {
  case WM_CREATE: {
    CreateControls(hwnd);
    return 0;
  }
  case WM_DESTROY:
    if (VideoMemDc) {
      EnterCriticalSection(&PaintCritSec);
      SelectObject(VideoMemDc, OldVideoMemDcObj);
      DeleteDC(VideoMemDc);
      DeleteObject(VideoBMP);
      VideoMemDc = nullptr;
      BMPWidth = 0;
      BMPHeight = 0;
      LeaveCriticalSection(&PaintCritSec);
    }
    if (!NoQuit) {
      av_log(nullptr, AV_LOG_INFO, "post quit\n");
      PostQuitMessage(0);
    }
    Hwnd = nullptr;
    SnapshotButton = nullptr;
    QualityTrackbar = nullptr;
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    EnterCriticalSection(&PaintCritSec);
    if (!VideoMemDc) {
      VideoMemDc = CreateCompatibleDC(dc);

      BITMAPINFO bi;
      ZeroMemory(&bi, sizeof(BITMAPINFO));
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = WindowWidth;
      bi.bmiHeader.biHeight = -WindowHeight; // negative so (0,0) is at top left
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;

      VideoBMP = CreateDIBSection(VideoMemDc, &bi, DIB_RGB_COLORS,
                                  (VOID **)&VideoBMPBits, nullptr, 0);
      OldVideoMemDcObj = SelectObject(VideoMemDc, VideoBMP);

      BMPWidth = WindowWidth;
      BMPHeight = WindowHeight;
    }

    BitBlt(dc, 0, 0, BMPWidth, BMPHeight, VideoMemDc, 0, 0, SRCCOPY);

    SetTextColor(dc, 0x00ffffff);
    SetBkColor(dc, 0x00000000);
    RECT textRect = {0, 0, BMPWidth, BMPHeight};

    wchar_t textBuf[64];
    snwprintf(textBuf, 64, L"%d.%01d kbits/s", Bitrate / 1000,
              (Bitrate % 1000) / 100);
    DrawText(dc, textBuf, -1, &textRect, DT_RIGHT | DT_TOP);

    LeaveCriticalSection(&PaintCritSec);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_DRAWITEM: {
    DefWindowProc(hwnd, uMsg, wParam, lParam);
    LPDRAWITEMSTRUCT di = (LPDRAWITEMSTRUCT)lParam;
    if (di->hwndItem == SnapshotButton) {
      DrawFrameControl(di->hDC, &di->rcItem, DFC_BUTTON,
                       DFCS_BUTTONPUSH |
                           ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
      DrawIcon(di->hDC, 7, 7, SnapIcon);
    }
    return TRUE;
  }
  case WM_COMMAND: {
    if (HWND(lParam) == SnapshotButton) {
      fprintf(stderr, "requesting snapshot\n");
      g_dispCtx->send_req_snapshot();
    }
    return 0;
  }
  case WM_LBUTTONUP: {
    int y = GET_Y_LPARAM(lParam);
    if (y < WindowHeight - 60) {
      fprintf(stderr, "requesting snapshot\n");
      g_dispCtx->send_req_snapshot();
    }
    return 0;
  }
  case WM_HSCROLL: {
    if (HWND(lParam) == QualityTrackbar) {
      switch (LOWORD(wParam)) {
      case TB_ENDTRACK: {
        DWORD dwPos = SendMessage(QualityTrackbar, TBM_GETPOS, 0, 0);
        if (dwPos > QUALITY_MAX) {
          dwPos = QUALITY_MAX;
          SendMessage(QualityTrackbar, TBM_SETPOS, TRUE, QUALITY_MAX);
        } else if (dwPos < QUALITY_MIN) {
          dwPos = QUALITY_MIN;
          SendMessage(QualityTrackbar, TBM_SETPOS, TRUE, QUALITY_MIN);
        }
        fprintf(stderr, "sending quality: %g\n", double(dwPos));
        g_dispCtx->send_quality(dwPos);
        break;
      }
      default:
        break;
      }
    }
    return 0;
  }
  case WM_MOVE: {
    RECT rect;
    GetWindowRect(hwnd, &rect);
    printf("window pos (%ld, %ld)\n", rect.left, rect.top);
    return 0;
  }
  default:
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

static WNDCLASS WindowClass = {0,       WindowProc,
                               0,       0,
                               nullptr, nullptr,
                               nullptr, (HBRUSH)COLOR_WINDOW,
                               nullptr, TEXT("tiberius-video")};
#define WindowStyle (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU)

static LRESULT CALLBACK SnapshotWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                                           LPARAM lParam) {
  switch (uMsg) {
  case WM_DESTROY:
    if (HDC video_mem_dc = (HDC)GetWindowLongPtr(hwnd, 0 * sizeof(LONG_PTR))) {
      HGDIOBJ old_video_mem_dc_obj =
          (HGDIOBJ)GetWindowLongPtr(hwnd, 1 * sizeof(LONG_PTR));
      HBITMAP video_bmp = (HBITMAP)GetWindowLongPtr(hwnd, 2 * sizeof(LONG_PTR));
      SelectObject(video_mem_dc, old_video_mem_dc_obj);
      DeleteDC(video_mem_dc);
      DeleteObject(video_bmp);
    }
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    EnterCriticalSection(&PaintCritSec);
    HDC video_mem_dc = (HDC)GetWindowLongPtr(hwnd, 0 * sizeof(LONG_PTR));
    if (!video_mem_dc) {
      video_mem_dc = CreateCompatibleDC(dc);

      BITMAPINFO bi;
      ZeroMemory(&bi, sizeof(BITMAPINFO));
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = SnapReqWidth;
      bi.bmiHeader.biHeight =
          -SnapReqHeight; // negative so (0,0) is at top left
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;

      LPVOID bits;
      HBITMAP video_bmp = CreateDIBSection(video_mem_dc, &bi, DIB_RGB_COLORS,
                                           &bits, nullptr, 0);
      memcpy(bits, SnapBits, SnapReqWidth * SnapReqHeight * 4);
      HGDIOBJ old_video_mem_dc_obj = SelectObject(video_mem_dc, video_bmp);

      SetWindowLongPtr(hwnd, 0 * sizeof(LONG_PTR), (LONG_PTR)video_mem_dc);
      SetWindowLongPtr(hwnd, 1 * sizeof(LONG_PTR),
                       (LONG_PTR)old_video_mem_dc_obj);
      SetWindowLongPtr(hwnd, 2 * sizeof(LONG_PTR), (LONG_PTR)video_bmp);
      SetWindowLongPtr(hwnd, 3 * sizeof(LONG_PTR), (LONG_PTR)SnapReqWidth);
      SetWindowLongPtr(hwnd, 4 * sizeof(LONG_PTR), (LONG_PTR)SnapReqHeight);
    }

    LONG_PTR width = GetWindowLongPtr(hwnd, 3 * sizeof(LONG_PTR));
    LONG_PTR height = GetWindowLongPtr(hwnd, 4 * sizeof(LONG_PTR));
    BitBlt(dc, 0, 0, width, height, video_mem_dc, 0, 0, SRCCOPY);

    LeaveCriticalSection(&PaintCritSec);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_LBUTTONUP:
    DestroyWindow(hwnd);
    return 0;
  default:
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

static WNDCLASS SnapshotWindowClass = {0,       SnapshotWindowProc,
                                       0,       5 * sizeof(LONG_PTR),
                                       nullptr, nullptr,
                                       nullptr, (HBRUSH)COLOR_WINDOW,
                                       nullptr, TEXT("tiberius-snapshot")};

static LRESULT CALLBACK WaitingWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                                          LPARAM lParam) {
  switch (uMsg) {
  case WM_DESTROY:
    if (!NoQuit) {
      av_log(nullptr, AV_LOG_INFO, "post quit\n");
      PostQuitMessage(0);
    }
    Hwnd = nullptr;
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    HICON icon =
        (HICON)LoadIcon(AppInstance, MAKEINTRESOURCE(IDI_NETWORK_TELEVISION));
    DrawIcon(dc, WAITING_WIDTH / 2 - 16, WAITING_HEIGHT * 3 / 4 / 2 - 16, icon);

    SetBkMode(dc, TRANSPARENT);
    wchar_t textBuf[64];
    snwprintf(textBuf, 64, L"Listening on %d.%d.%d.%d:%d",
              SdpAddr.sin_addr.S_un.S_un_b.s_b1,
              SdpAddr.sin_addr.S_un.S_un_b.s_b2,
              SdpAddr.sin_addr.S_un.S_un_b.s_b3,
              SdpAddr.sin_addr.S_un.S_un_b.s_b4, SdpPort);
    RECT textRect = {0, WAITING_HEIGHT / 2, WAITING_WIDTH, WAITING_HEIGHT};
    DrawText(dc, textBuf, -1, &textRect, DT_CENTER | DT_VCENTER);

    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_MOVE: {
    RECT rect;
    GetWindowRect(hwnd, &rect);
    printf("window pos (%ld, %ld)\n", rect.left, rect.top);
    return 0;
  }
  default:
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

static WNDCLASS WaitingWindowClass = {0,       WaitingWindowProc,
                                      0,       0,
                                      nullptr, nullptr,
                                      nullptr, (HBRUSH)COLOR_WINDOW,
                                      nullptr, TEXT("tiberius-waiting")};

static void CreateSnapshotButton(HWND parent) {
  if (SnapshotButton)
    DestroyWindow(SnapshotButton);

  SnapshotButton =
      CreateWindow(WC_BUTTON, TEXT(""), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,
                   WindowHeight, 30, 30, parent, nullptr, AppInstance, nullptr);
}

static void CreateQualityTrackbar(HWND parent) {
  if (QualityTrackbar)
    DestroyWindow(QualityTrackbar);

  QualityTrackbar =
      CreateWindow(TRACKBAR_CLASS, TEXT("Video Quality"),
                   WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 30, WindowHeight,
                   WindowWidth - 30, 30, parent, nullptr, AppInstance, nullptr);

  SendMessage(QualityTrackbar, TBM_SETRANGE, TRUE,
              MAKELONG(QUALITY_MIN, QUALITY_MAX));
  SendMessage(QualityTrackbar, TBM_SETPAGESIZE, 0, 4);
  SendMessage(QualityTrackbar, TBM_SETPOS, TRUE, QUALITY_DEF);
}

static void CreateControls(HWND parent) {
  CreateSnapshotButton(parent);
  CreateQualityTrackbar(parent);
}

static RECT MakeWindowRect(int x, int y, int width, int height) {
  RECT rect = {0, 0, width, height};
  AdjustWindowRectEx(&rect, WindowStyle, FALSE, 0);
  if (rect.left < 0) {
    rect.right -= rect.left;
    rect.left = 0;
  }
  if (rect.top < 0) {
    rect.bottom -= rect.top;
    rect.top = 0;
  }

  rect.left += x;
  rect.right += x;
  rect.top += y;
  rect.bottom += y;

  return rect;
}

static void GetCommentXY(LONG &x, LONG &y) {
  if (char *p = strstr(Comment, "x:"))
    x = strtol(p + 2, nullptr, 10);
  if (char *p = strstr(Comment, "y:"))
    y = strtol(p + 2, nullptr, 10);
}

static void ReinitWindow() {
  LONG x = 0, y = 0;
  if (Hwnd) {
    RECT rect;
    if (GetWindowRect(Hwnd, &rect)) {
      x = rect.left;
      y = rect.top;
    } else {
      GetCommentXY(x, y);
    }
    NoQuit = true;
    DestroyWindow(Hwnd);
    NoQuit = false;
  } else {
    GetCommentXY(x, y);
  }

  WindowWidth = ReqWidth;
  WindowHeight = ReqHeight;

  wchar_t windowTitleBuf[128];
  wchar_t *windowTitle = nullptr;
  if (Title) {
    MultiByteToWideChar(CP_ACP, 0, Title, -1, windowTitleBuf, 128);
    windowTitle = windowTitleBuf;
  }

  RECT rect = MakeWindowRect(x, y, WindowWidth, WindowHeight + 30);

  Hwnd = CreateWindow(
      TEXT("tiberius-video"), windowTitle ? windowTitle : L"NO NAME",
      WindowStyle | WS_VISIBLE, rect.left, rect.top, rect.right - rect.left,
      rect.bottom - rect.top, nullptr, nullptr, AppInstance, nullptr);
  SetWindowPos(Hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

static void HandleFrame() {
  EnterCriticalSection(&PaintCritSec);
  if (WindowWidth != ReqWidth || WindowHeight != ReqHeight)
    ReinitWindow();
  else
    InvalidateRgn(Hwnd, nullptr, FALSE);
  LeaveCriticalSection(&PaintCritSec);
}

static void HandleSnapshotFrame() {
  EnterCriticalSection(&PaintCritSec);
  if (SnapPending) {
    wchar_t windowTitleBuf[128];
    wchar_t *windowTitle = nullptr;
    if (Title) {
      wchar_t windowTitleConv[128];
      MultiByteToWideChar(CP_ACP, 0, Title, -1, windowTitleConv, 128);
      snwprintf(windowTitleBuf, 128, L"%s Snapshot", windowTitleConv);
      windowTitle = windowTitleBuf;
    }

    RECT rect = MakeWindowRect(0, 0, SnapReqWidth, SnapReqHeight);

    HWND h = CreateWindow(
        TEXT("tiberius-snapshot"), windowTitle ? windowTitle : L"NO NAME",
        WindowStyle | WS_VISIBLE, rect.left, rect.top, rect.right - rect.left,
        rect.bottom - rect.top, nullptr, nullptr, AppInstance, nullptr);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  }
  LeaveCriticalSection(&PaintCritSec);
}

static void HandleAddrInfo() {
  wchar_t windowTitleBuf[128];
  wchar_t *windowTitle = nullptr;
  if (Title) {
    MultiByteToWideChar(CP_ACP, 0, Title, -1, windowTitleBuf, 128);
    windowTitle = windowTitleBuf;
  }

  LONG x = 0, y = 0;
  GetCommentXY(x, y);

  RECT rect = MakeWindowRect(x, y, WAITING_WIDTH, WAITING_HEIGHT);

  Hwnd = CreateWindow(
      TEXT("tiberius-waiting"), windowTitle ? windowTitle : L"NO NAME",
      WindowStyle | WS_VISIBLE, rect.left, rect.top, rect.right - rect.left,
      rect.bottom - rect.top, nullptr, nullptr, AppInstance, nullptr);
  SetWindowPos(Hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

static int LaunchFoundSDPs(HINSTANCE hInstance) {
  WCHAR module_path[MAX_PATH];
  GetModuleFileName(hInstance, module_path, MAX_PATH);

  WCHAR search_pattern[MAX_PATH];
  wcsncpy(search_pattern, module_path, MAX_PATH);
  WCHAR *last_slash = wcsrchr(search_pattern, L'\\');
  wcscpy(last_slash, L"\\*.sdp");
  WIN32_FIND_DATA find_data;
  HANDLE find = FindFirstFile(search_pattern, &find_data);
  *last_slash = L'\0';
  if (find != INVALID_HANDLE_VALUE) {
    do {
      WCHAR cmd_line[MAX_PATH];
      snwprintf(cmd_line, MAX_PATH, L"\"%s\\%s\"", search_pattern,
                find_data.cFileName);
      STARTUPINFO si{};
      si.cb = sizeof(si);
      PROCESS_INFORMATION pi{};
      CreateProcess(module_path, cmd_line, nullptr, nullptr, FALSE, 0, nullptr,
                    nullptr, &si, &pi);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
    } while (FindNextFile(find, &find_data));
    FindClose(find);
    return 0;
  }

  MessageBox(nullptr, TEXT("No .sdp files found next to executable"),
             TEXT("No SDP"), MB_OK | MB_ICONERROR);
  return 1;
}

// -DCMAKE_C_COMPILER=/opt/x86mingw32ce/bin/i386-mingw32ce-gcc
// -DCMAKE_CXX_COMPILER=/opt/x86mingw32ce/bin/i386-mingw32ce-g++

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPTSTR lpCmdLine, int nShowCmd) {
  AppInstance = hInstance;
  SnapIcon = (HICON)LoadImage(AppInstance, MAKEINTRESOURCE(IDI_CAMERA),
                              IMAGE_ICON, 16, 16, 0);
  InitCommonControls();

  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    MessageBox(nullptr, TEXT("Failed to set up sockets"), TEXT("No Network"),
               MB_OK | MB_ICONERROR);
    return 1;
  }

  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(lpCmdLine, &argc);
  if (argc == 0)
    return LaunchFoundSDPs(hInstance);

  MainThreadId = GetCurrentThreadId();
  InitializeCriticalSection(&PaintCritSec);
  HANDLE VideoThread = CreateThread(nullptr, 0, VideoTask, argv[0], 0, nullptr);

  WindowClass.hInstance = hInstance;
  RegisterClass(&WindowClass);

  SnapshotWindowClass.hInstance = hInstance;
  RegisterClass(&SnapshotWindowClass);

  WaitingWindowClass.hInstance = hInstance;
  RegisterClass(&WaitingWindowClass);

  while (g_running) {
    MSG msg;
    if (GetMessage(&msg, nullptr, 0, 0)) {
      if (msg.message == WM_USER) {
        HandleFrame();
      } else if (msg.message == WM_USER + 1) {
        HandleSnapshotFrame();
      } else if (msg.message == WM_USER + 2) {
        HandleAddrInfo();
      } else {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
    } else {
      g_running = false;
      av_log(nullptr, AV_LOG_INFO, "received quit\n");
      break;
    }
  }

  av_log(nullptr, AV_LOG_INFO, "waiting for exit\n");
  WaitForSingleObject(VideoThread, 2000);
  WSACleanup();
  av_log(nullptr, AV_LOG_INFO, "exiting\n");

  return 0;
}
