#pragma once
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

// ── Shared state (defined in NexMatch_Core.cpp) ────────────────────────────────
extern std::atomic<bool>      g_cs2Running;
extern std::atomic<int>       g_ssCount;
extern std::atomic<int>       g_uploadCount;
extern std::wstring           g_sessionFolder;
extern std::wstring           g_steamId;
extern std::wstring           g_pcName;
extern bool                   g_sessionActive;
extern int                    g_screenshotCountdown;
extern int                    g_uploadCountdown;
extern int                    g_heartbeatCountdown;
extern std::vector<std::wstring> g_pendingFiles;
extern std::mutex             g_filesMutex;

// ── Upload payload ─────────────────────────────────────────────────────────────
struct UploadPayload {
    std::string steamId, pcName, timestamp;
    std::vector<std::string>  imageFiles;
    std::vector<std::wstring> filePaths;
};

// ── Core API (implemented in .lib / .dll) ──────────────────────────────────────
std::wstring GetSteamID();
std::wstring GetPCName();
std::wstring CreateSessionFolder();
bool         IsCS2Running();
std::wstring SaveScreenshotToFile();
void         TriggerHeartbeat();
void         TriggerUpload();

// ── Time utilities ─────────────────────────────────────────────────────────────
std::wstring DateStr();
std::wstring TimeStr();
std::string  TimeStrA();
