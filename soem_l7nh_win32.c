// soem_l7nh_win32.c
// Simple Windows GUI program using SOEM to operate LS Mecapion L7NH in Cyclic Synchronous Torque (CST) mode.
// - Requires SOEM built for Windows and accessible include/library paths (ethercat.h, soem.lib)
// - Build with CMake + Visual Studio Build Tools
// - This is a demonstration: PDO/units may differ per ESI. Verify object indexes/subindexes for your drive.

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "ethercat.h"   // from SOEM: make sure include path is set

#define EC_TIMEOUTMON 500
#define DRIVE_SLAVE 1   // using first discovered slave (adjust if you have multiple)

// CiA402 object indexes
#define IDX_CONTROLWORD 0x6040
#define IDX_STATUSWORD  0x6041
#define IDX_MODE_OF_OPERATION 0x6060
#define IDX_MODE_OF_OPERATION_DISPLAY 0x6061
#define IDX_TARGET_TORQUE 0x6071
#define IDX_ACTUAL_TORQUE 0x6077
#define IDX_ACTUAL_VELOCITY 0x606C

// Controlword commands (common CiA402 masks/values)
#define CW_SHUTDOWN 0x0006
#define CW_SWITCH_ON 0x0007
#define CW_ENABLE_OPERATION 0x000F
#define CW_QUICK_STOP 0x0002
#define CW_DISABLE_VOLTAGE 0x0000
#define CW_FAULT_RESET 0x0080

static HWND hWndMain = NULL, hBtnStart = NULL, hBtnStop = NULL, hStaticRPM = NULL;
static HANDLE hThread = NULL;
static volatile bool run_flag = false;
static char ifname[128] = ""; // network interface name (set by command line or edit here)

// Forward
DWORD WINAPI EtherCATThread(LPVOID lpParam);

// Helper: set static text
void SetRPMText(const char *txt) {
    SetWindowTextA(hStaticRPM, txt);
}

// Win32 callbacks
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        hBtnStart = CreateWindowA("BUTTON", "Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            20, 20, 100, 30, hwnd, (HMENU)1, NULL, NULL);
        hBtnStop = CreateWindowA("BUTTON", "Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            140, 20, 100, 30, hwnd, (HMENU)2, NULL, NULL);
        hStaticRPM = CreateWindowA("STATIC", "RPM: -", WS_CHILD | WS_VISIBLE | SS_SIMPLE,
            20, 70, 360, 24, hwnd, NULL, NULL, NULL);
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == 1) { // Start
            if (!run_flag) {
                run_flag = true;
                hThread = CreateThread(NULL, 0, EtherCATThread, NULL, 0, NULL);
            }
        } else if (LOWORD(wParam) == 2) { // Stop
            if (run_flag) {
                run_flag = false;
                // wait for thread to exit
                if (hThread) {
                    WaitForSingleObject(hThread, 2000);
                    CloseHandle(hThread);
                    hThread = NULL;
                }
            }
        }
        break;
    case WM_DESTROY:
        run_flag = false;
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// EtherCAT helper: write SDO (wrap ec_SDOwrite)
int write_sdo_u8(uint16 slave, uint16 idx, uint8 sub, uint8 val) {
    return ec_SDOwrite(slave, idx, sub, FALSE, sizeof(uint8), &val, EC_TIMEOUTRXM);
}
int write_sdo_u16(uint16 slave, uint16 idx, uint8 sub, uint16 val) {
    return ec_SDOwrite(slave, idx, sub, FALSE, sizeof(uint16), &val, EC_TIMEOUTRXM);
}
int write_sdo_s32(uint16 slave, uint16 idx, uint8 sub, int32_t val) {
    return ec_SDOwrite(slave, idx, sub, FALSE, sizeof(int32_t), &val, EC_TIMEOUTRXM);
}
int read_sdo_s32(uint16 slave, uint16 idx, uint8 sub, int32_t *out) {
    int size = sizeof(int32_t);
    int ret = ec_SDOread(slave, idx, sub, FALSE, &size, out, EC_TIMEOUTRXM);
    return ret;
}

// Thread: initialize SOEM and run simple control loop
DWORD WINAPI EtherCATThread(LPVOID lpParam) {
    int i, j;
    int slavecount;
    char txt[256];

    // if ifname empty, prompt user with message box (can't input here); assume "eth0" or ask to set
    if (ifname[0] == '\0') {
        // default: try to find first interface listed by SOEM tutorial: Windows often uses "eth0" or "Ethernet"
        // User should set command line arg. We'll try "eth0" then "Ethernet".
        strcpy_s(ifname, sizeof(ifname), "eth0");
    }

    if (!ec_init(ifname)) {
        sprintf_s(txt, sizeof(txt), "ec_init on interface '%s' failed. Is interface name correct and EtherCAT cable connected?", ifname);
        SetRPMText(txt);
        run_flag = false;
        return 1;
    }

    // find and configure slaves
    if (ec_config_init(FALSE) <= 0) {
        SetRPMText("No slaves found or config init failed");
        ec_close();
        run_flag = false;
        return 1;
    }

    slavecount = ec_slavecount;
    sprintf_s(txt, sizeof(txt), "Found %d slaves", slavecount);
    SetRPMText(txt);

    // Map process data (basic)
    ec_config_map(NULL);
    ec_configdc();

    // change to operational
    ec_statecheck(0, EC_STATE_SAFE_OP,  EC_TIMEOUTSTATE);
    ec_slave[DRIVE_SLAVE].state = EC_STATE_OPERATIONAL;
    ec_writestate(DRIVE_SLAVE);
    ec_statecheck(DRIVE_SLAVE, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);

    if (ec_slave[DRIVE_SLAVE].state != EC_STATE_OPERATIONAL) {
        SetRPMText("Failed to reach OPERATIONAL state");
        ec_close();
        run_flag = false;
        return 1;
    }

    SetRPMText("Operational - configuring drive via SDOs...");

    // 1) set Mode of Operation to Cyclic Synchronous Torque (CST). CiA402 value 10.
    uint8 mode = 10; // CST
    if (write_sdo_u8(DRIVE_SLAVE, IDX_MODE_OF_OPERATION, 0x00, mode) <= 0) {
        SetRPMText("Failed to write Mode of Operation (0x6060)");
        // continue anyway
    }

    Sleep(50);

    // 2) State machine via controlword (0x6040) -- using SDO writes for demo (many drives accept controlword via PDO; using SDO may work for commissioning)
    // Shutdown
    if (write_sdo_u16(DRIVE_SLAVE, IDX_CONTROLWORD, 0x00, (uint16)CW_SHUTDOWN) <= 0) {
        // ignore
    }
    Sleep(100);
    // Switch on
    write_sdo_u16(DRIVE_SLAVE, IDX_CONTROLWORD, 0x00, (uint16)CW_SWITCH_ON);
    Sleep(100);
    // Enable operation
    write_sdo_u16(DRIVE_SLAVE, IDX_CONTROLWORD, 0x00, (uint16)CW_ENABLE_OPERATION);
    Sleep(200);

    SetRPMText("Drive enabled - applying torque setpoint...");

    // Main loop: while run_flag, set a small torque command and read actual velocity
    int32_t torque_set = 1000; // unit: drive dependent (tune carefully!). Use safe small value.

    while (run_flag) {
        // write target torque (0x6071)
        write_sdo_s32(DRIVE_SLAVE, IDX_TARGET_TORQUE, 0x00, torque_set);

        // read actual velocity (0x606C). Many drives return velocity in [rpm] or [units]. Check your ESI/manual.
        int32_t vel_raw = 0;
        if (read_sdo_s32(DRIVE_SLAVE, IDX_ACTUAL_VELOCITY, 0x00, &vel_raw) > 0) {
            // convert if needed; here assume raw value equals RPM. If not, user must apply proper scale from manual.
            sprintf_s(txt, sizeof(txt), "RPM: %d (raw)", vel_raw);
            SetRPMText(txt);
        } else {
            SetRPMText("Could not read actual velocity (0x606C)");
        }

        // simple 100 ms loop
        Sleep(100);
    }

    // On stop: set torque zero and request disable
    write_sdo_s32(DRIVE_SLAVE, IDX_TARGET_TORQUE, 0x00, 0);
    // quick stop
    write_sdo_u16(DRIVE_SLAVE, IDX_CONTROLWORD, 0x00, (uint16)CW_QUICK_STOP);
    Sleep(100);
    // shutdown
    write_sdo_u16(DRIVE_SLAVE, IDX_CONTROLWORD, 0x00, (uint16)CW_SHUTDOWN);

    // read last velocity to show final RPM
    int32_t last_vel = 0;
    if (read_sdo_s32(DRIVE_SLAVE, IDX_ACTUAL_VELOCITY, 0x00, &last_vel) > 0) {
        sprintf_s(txt, sizeof(txt), "Final RPM: %d (raw)", last_vel);
        SetRPMText(txt);
    } else {
        SetRPMText("Stopped - final RPM unknown");
    }

    // close
    ec_close();
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    MSG Msg;
    WNDCLASSEXA wc;

    // If user passed interface name as command line, copy it
    if (lpCmdLine && lpCmdLine[0] != '\0') {
        strncpy_s(ifname, sizeof(ifname), lpCmdLine, _TRUNCATE);
    }

    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = "SOEM_L7NH_Class";
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Window Registration Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    hWndMain = CreateWindowA("SOEM_L7NH_Class", "SOEM L7NH Demo", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 420, 160, NULL, NULL, hInstance, NULL);

    if (hWndMain == NULL) {
        MessageBoxA(NULL, "Window Creation Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    ShowWindow(hWndMain, nCmdShow);
    UpdateWindow(hWndMain);

    while (GetMessage(&Msg, NULL, 0, 0) > 0) {
        TranslateMessage(&Msg);
        DispatchMessage(&Msg);
    }
    return (int)Msg.wParam;
}
