/*
 * L7NH 서보 드라이브 이더캣 컨트롤러 (간소화 구현)
 * SOEM 라이브러리를 사용하여 L7NH 서보 드라이브를 토크 모드로 제어
 * 시작/정지 버튼과 RPM 표시가 있는 윈도우 GUI 애플리케이션
 */

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>

// SOEM 기능에 대한 전방 선언
// 실제 구현에서는 SOEM 헤더에서 가져옴
typedef struct {
    uint16_t state;
    uint16_t ALstatuscode;
} ec_slave_t;

// 시뮬레이션을 위한 간소화된 전역 변수
static HWND hConnectButton = NULL;
static HWND hStartButton = NULL;
static HWND hStopButton = NULL;
static HWND hRPMLabel = NULL;
static HWND hStatusLabel = NULL;
static HWND hMainWnd = NULL;
static BOOL isConnected = FALSE;
static BOOL inOperation = FALSE;
static int32_t currentRPM = 0;
static int32_t targetTorque = 100;  // 0.1% 단위의 10.0% 토크

// 창 크기
#define WINDOW_WIDTH 450
#define WINDOW_HEIGHT 300
#define BUTTON_WIDTH 80
#define BUTTON_HEIGHT 30
#define LABEL_HEIGHT 25

// 제어 ID
#define IDC_CONNECT_BUTTON 100
#define IDC_START_BUTTON 101
#define IDC_STOP_BUTTON 102
#define IDC_RPM_LABEL 103
#define IDC_STATUS_LABEL 104

// CiA 402 토크 제어 상수
#define MODE_TORQUE          0x04
#define CW_SHUTDOWN          0x0006
#define CW_SWITCH_ON         0x0007
#define CW_ENABLE_OPERATION  0x000F
#define CW_QUICK_STOP        0x0002

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
DWORD WINAPI SimulationThreadProc(LPVOID lpParam);
BOOL ConnectToServo();

// 윈도우 애플리케이션의 메인 진입점
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // 일반적인 컨트롤 초기화
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    // 창 클래스 등록
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"EthercatServoControl";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpszMenuName = NULL;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, L"Window Registration Failed!", L"Error!", MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }

    // 중심에 창 위치 계산
    int x = (GetSystemMetrics(SM_CXSCREEN) - WINDOW_WIDTH) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - WINDOW_HEIGHT) / 2;

    // 창 생성
    hMainWnd = CreateWindowEx(
        0,
        L"EthercatServoControl",
        L"L7NH 서보 드라이브 제어 (토크 모드)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, WINDOW_WIDTH, WINDOW_HEIGHT,
        NULL, NULL, hInstance, NULL
    );

    if (hMainWnd == NULL) {
        MessageBox(NULL, L"Window Creation Failed!", L"Error!", MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }

    // 창 표시 및 업데이트
    ShowWindow(hMainWnd, nCmdShow);
    UpdateWindow(hMainWnd);

    // 시뮬레이션 스레드 생성
    HANDLE hSimulationThread = CreateThread(NULL, 0, SimulationThreadProc, NULL, 0, NULL);
    if (hSimulationThread == NULL) {
        MessageBox(hMainWnd, L"Failed to create simulation thread", L"Error", MB_OK);
    } else {
        CloseHandle(hSimulationThread);
    }

    // 메시지 루프
    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_CREATE:
        {
            // 컨트롤 생성
            hConnectButton = CreateWindow(
                L"BUTTON",
                L"\xacfc\uc18d",  // Connect
                WS_TABSTOP | WS_VISIBLE | WS_CHILD,
                50, 20, BUTTON_WIDTH, BUTTON_HEIGHT,
                hwnd, (HMENU)IDC_CONNECT_BUTTON, ((LPCREATESTRUCT)lParam)->hInstance, NULL
            );

            hStartButton = CreateWindow(
                L"BUTTON",
                L"\uc2dc\uc791",  // Start
                WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                145, 20, BUTTON_WIDTH, BUTTON_HEIGHT,
                hwnd, (HMENU)IDC_START_BUTTON, ((LPCREATESTRUCT)lParam)->hInstance, NULL
            );
            
            hStopButton = CreateWindow(
                L"BUTTON",
                L"\uc885\ub8cc",  // Stop
                WS_TABSTOP | WS_VISIBLE | WS_CHILD,
                240, 20, BUTTON_WIDTH, BUTTON_HEIGHT,
                hwnd, (HMENU)IDC_STOP_BUTTON, ((LPCREATESTRUCT)lParam)->hInstance, NULL
            );
            
            CreateWindow(
                L"STATIC",
                L"\ud604\uc7ac RPM:",  // Current RPM:
                WS_VISIBLE | WS_CHILD,
                50, 80, 100, LABEL_HEIGHT,
                hwnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL
            );
            
            hRPMLabel = CreateWindow(
                L"STATIC",
                L"0 RPM",
                WS_VISIBLE | WS_CHILD | SS_CENTER,
                150, 80, 100, LABEL_HEIGHT,
                hwnd, (HMENU)IDC_RPM_LABEL, ((LPCREATESTRUCT)lParam)->hInstance, NULL
            );
            
            CreateWindow(
                L"STATIC",
                L"\uc0c1\ud0dc:",  // Status:
                WS_VISIBLE | WS_CHILD,
                50, 120, 100, LABEL_HEIGHT,
                hwnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL
            );
            
            hStatusLabel = CreateWindow(
                L"STATIC",
                L"\uc5f0\uacb0 \ub2e8\ud0c0 - \uc5f0\uacb0 \ubc84\ud2bc \ud074\ub9ad",  // Disconnected - Click Connect
                WS_VISIBLE | WS_CHILD | SS_LEFT,
                50, 150, 350, LABEL_HEIGHT,
                hwnd, (HMENU)IDC_STATUS_LABEL, ((LPCREATESTRUCT)lParam)->hInstance, NULL
            );
            
            break;
        }

        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                case IDC_CONNECT_BUTTON:
                    if (!isConnected) {
                        // L7NH 서보 드라이브 연결 시도
                        if (ConnectToServo()) {
                            isConnected = TRUE;
                            SetWindowText(hStatusLabel, L"L7NH \uc11c\ubcf8 \ub4dc\ub77c\uc774\ube0c\uc5d0 \uc5f0\uacb0\ub428 - \uc2dc\uc791 \ubc84\ud2bc\uc744 \ud074\ub9ad\ud558\uc138\uc694");  // Connected to L7NH servo drive - Click Start button
                        } else {
                            SetWindowText(hStatusLabel, L"\uc5f0\uacb0 \uc2e4\ud328 - \uc774\ub354\uce75 \ub124\ud2b8\uc6cc\ud0a4\ub97c \ud655\uc778\ud558\uc138\uc694");  // Connection failed - Check EtherCAT network
                        }
                    } else {
                        SetWindowText(hStatusLabel, L"\uc774\ubbf8 \uc5f0\uacb0\ub428 - \uc791\ub3d9\uc744 \uc2dc\uc791\ud558\ub824\uba74 \uc2dc\uc791 \ubc84\ud2bc\uc744 \ud074\ub9ad\ud558\uc138\uc694");  // Already connected - Click Start to operate
                    }
                    break;

                case IDC_START_BUTTON:
                    if (isConnected) {
                        inOperation = TRUE;
                        SetWindowText(hStatusLabel, L"\uc11c\ubcf8\uc774 \ud1b5\uc7a5 \ubaa8\ub4dc\ub85c \uc2dc\uc791\ub428");  // Servo started in torque mode
                    } else {
                        SetWindowText(hStatusLabel, L"\uc5f0\uacb0\ub418\uc9c0 \uc54a\uc74c - \ub9c8\uc9c0\ub9c9 \uc5f0\uacb0\ud558\uc138\uc694");  // Not connected - Please connect first
                    }
                    break;

                case IDC_STOP_BUTTON:
                    inOperation = FALSE;
                    SetWindowText(hStatusLabel, L"\uc11c\ubcf8 \uc885\ub8cc\ub428");  // Servo stopped
                    break;
            }
            break;
        }

        case WM_CLOSE:
        {
            // 작동 중인 서보 정지
            if (inOperation) {
                inOperation = FALSE;
            }

            DestroyWindow(hwnd);
            break;
        }

        case WM_DESTROY:
        {
            PostQuitMessage(0);
            break;
        }

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

// 서보 드라이브 연결을 시뮬레이션하는 함수
BOOL ConnectToServo()
{
    // 실제 구현에서는 이더캣 통신을 통해 L7NH 드라이브에 연결 초기화
    // 시뮬레이션에서는 TRUE 반환으로 연결 성공 표시
    // 실제 애플리케이션에서는 SOEM 함수 호출로 슬레이브 감지 및 구성
    return TRUE;
}

DWORD WINAPI SimulationThreadProc(LPVOID lpParam)
{
    int counter = 0;

    while (TRUE) {
        if (inOperation && isConnected) {
            // 토크 제어에 따른 RPM 시뮬레이션
            // 실제 애플리케이션에서는 서보와 이더캣을 통해 통신
            currentRPM = (int32_t)(targetTorque * 50.0 * sin(counter * 0.1)); // 시뮬레이션 RPM
            
            // RPM 디스플레이 업데이트
            wchar_t rpmText[32];
            swprintf_s(rpmText, 32, L"%d RPM", currentRPM);
            
            // 메인 스레드에서 RPM 레이블 업데이트
            SendMessage(hRPMLabel, WM_SETTEXT, 0, (LPARAM)rpmText);
        } else if (inOperation && !isConnected) {
            // 연결되지 않은 상태에서 작동 시도 시 오류 표시
            SetWindowText(hStatusLabel, L"\uc5f0\uacb0\ub418\uc9c0 \uc54a\uc74c - \uc11c\ubcf8\uc744 \uc2dc\uc791\ud560 \uc218 \uc5c6\uc2b5\ub2c8\ub2e4");  // Not connected - Cannot start servo
            inOperation = FALSE;
        } else {
            // 정지 시 천천히 감속
            if (abs(currentRPM) > 10) {
                currentRPM = (int32_t)(currentRPM * 0.95);
            } else {
                currentRPM = 0;
            }
            
            // 정지 시에도 RPM 디스플레이 업데이트
            wchar_t rpmText[32];
            swprintf_s(rpmText, 32, L"%d RPM", currentRPM);
            SendMessage(hRPMLabel, WM_SETTEXT, 0, (LPARAM)rpmText);
        }
        
        counter++;
        Sleep(50); // 50ms마다 업데이트
    }
    
    return 0;
}