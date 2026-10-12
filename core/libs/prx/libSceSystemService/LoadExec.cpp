#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <system_error>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#include "prx/libSceSystemService/LoadExec.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path ResolveExecutable(const char* guestPath, bool& restart) {
    restart = std::strcmp(guestPath, "/app0/eboot.bin") == 0;
    if (restart) return {};
    std::string normalized(guestPath);
    for (auto& character : normalized) if (character == '\\') character = '/';
    if (!normalized.starts_with("/app0/") || normalized.find("/../") != std::string::npos || normalized.ends_with("/.."))
        throw std::runtime_error("sceSystemServiceLoadExec: executable is not a converted app0 path: " + std::string(guestPath));
    std::filesystem::path resolved;
    try {
        resolved = ResolvePath_nid_no_patch(normalized.c_str());
    } catch (const std::exception& error) {
        throw std::runtime_error("sceSystemServiceLoadExec: cannot resolve " + std::string(guestPath) + ": " + error.what());
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(resolved, error))
        throw std::runtime_error("sceSystemServiceLoadExec: converted executable not found for " + std::string(guestPath));
    return resolved;
}

std::vector<std::string> CopyArguments(const char* const* arguments) {
    std::vector<std::string> result;
    if (arguments != nullptr) for (auto argument = arguments; *argument != nullptr; ++argument) result.emplace_back(*argument);
    return result;
}

}

#ifdef _WIN32

namespace {

constexpr std::size_t CommandLineCapacity = 32767;
constexpr wchar_t PreviousProcessVariable[] = L"ANYPS5_LOAD_EXEC_PREVIOUS_PROCESS";

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) throw std::length_error("sceSystemServiceLoadExec: argument too long");
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size == 0) throw std::invalid_argument("sceSystemServiceLoadExec: argument is not UTF-8");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size) != size)
        throw std::invalid_argument("sceSystemServiceLoadExec: argument is not UTF-8");
    return result;
}

void AppendArgument(std::wstring& commandLine, const std::wstring& argument) {
    if (!commandLine.empty()) commandLine += L' ';
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        commandLine += argument;
        return;
    }
    commandLine += L'"';
    std::size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        commandLine.append(character == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        commandLine += character;
        backslashes = 0;
    }
    commandLine.append(backslashes * 2, L'\\');
    commandLine += L'"';
}

std::wstring CurrentExecutablePath() {
    std::wstring path(CommandLineCapacity, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "sceSystemServiceLoadExec: executable path");
    if (size >= path.size()) throw std::length_error("sceSystemServiceLoadExec: executable path too long");
    path.resize(size);
    return path;
}

class PreviousProcessWait final {
public:
    PreviousProcessWait() {
        wchar_t value[32]{};
        const auto length = GetEnvironmentVariableW(PreviousProcessVariable, value, static_cast<DWORD>(std::size(value)));
        if (length == 0) return;
        if (_wputenv_s(PreviousProcessVariable, L"") != 0) throw std::runtime_error("sceSystemServiceLoadExec: cannot clear the previous process variable");
        if (length >= std::size(value)) throw std::runtime_error("sceSystemServiceLoadExec: invalid previous process handle");
        wchar_t* end = nullptr;
        const auto raw = std::wcstoull(value, &end, 10);
        if (end == value || *end != L'\0') throw std::runtime_error("sceSystemServiceLoadExec: invalid previous process handle");
        const auto handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw));
        const auto waited = WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
        if (waited != WAIT_OBJECT_0) throw std::runtime_error("sceSystemServiceLoadExec: waiting for the previous process failed");
    }
};

const PreviousProcessWait previousProcessWait;

}

[[noreturn]] void SystemService::ReplaceProcess(const char* path, const char* const* arguments) {
    bool restart = false;
    const auto resolved = ResolveExecutable(path, restart);
    const auto executable = restart ? CurrentExecutablePath() : resolved.wstring();
    const auto copiedArguments = CopyArguments(arguments);
    std::wstring commandLine;
    AppendArgument(commandLine, executable);
    for (const auto& argument : copiedArguments) AppendArgument(commandLine, Widen(argument));
    if (commandLine.size() >= CommandLineCapacity) throw std::length_error("sceSystemServiceLoadExec: command line too long");

    std::wstring previousPath(CommandLineCapacity, L'\0');
    const auto pathLength = GetEnvironmentVariableW(L"PATH", previousPath.data(), static_cast<DWORD>(previousPath.size()));
    if (pathLength >= previousPath.size()) throw std::length_error("sceSystemServiceLoadExec: PATH too long");
    previousPath.resize(pathLength);
    const auto runtimeDirectory = std::filesystem::path(CurrentExecutablePath()).parent_path().wstring();
    const auto childPath = previousPath.empty() ? runtimeDirectory : runtimeDirectory + L";" + previousPath;
    if (!SetEnvironmentVariableW(L"PATH", childPath.c_str()))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "sceSystemServiceLoadExec: runtime library path");
    HANDLE self = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self, SYNCHRONIZE, TRUE, 0)) {
        const auto error = GetLastError();
        if (previousPath.empty()) SetEnvironmentVariableW(L"PATH", nullptr);
        else SetEnvironmentVariableW(L"PATH", previousPath.c_str());
        throw std::system_error(static_cast<int>(error), std::system_category(), "sceSystemServiceLoadExec: process handle");
    }
    if (!SetEnvironmentVariableW(PreviousProcessVariable, std::to_wstring(reinterpret_cast<std::uintptr_t>(self)).c_str())) {
        const auto error = GetLastError();
        CloseHandle(self);
        if (previousPath.empty()) SetEnvironmentVariableW(L"PATH", nullptr);
        else SetEnvironmentVariableW(L"PATH", previousPath.c_str());
        throw std::system_error(static_cast<int>(error), std::system_category(), "sceSystemServiceLoadExec: previous process variable");
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto created = CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
    const auto error = GetLastError();
    SetEnvironmentVariableW(PreviousProcessVariable, nullptr);
    if (previousPath.empty()) SetEnvironmentVariableW(L"PATH", nullptr);
    else SetEnvironmentVariableW(L"PATH", previousPath.c_str());
    if (!created) {
        CloseHandle(self);
        throw std::system_error(static_cast<int>(error), std::system_category(), "sceSystemServiceLoadExec: starting " + std::string(path));
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const auto resumeError = GetLastError();
        TerminateProcess(process.hProcess, static_cast<UINT>(resumeError));
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(self);
        throw std::system_error(static_cast<int>(resumeError), std::system_category(), "sceSystemServiceLoadExec: resuming " + std::string(path));
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    LibcRunShutdown_nid_postfix();
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), 0);
    std::_Exit(0);
}

#else

namespace {

std::string CurrentExecutablePath() {
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) throw std::runtime_error("sceSystemServiceLoadExec: executable path");
    path.resize(std::strlen(path.c_str()));
    return std::filesystem::weakly_canonical(path).string();
#else
    std::vector<char> path(32768);
    const auto size = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (size < 0) throw std::system_error(errno, std::generic_category(), "sceSystemServiceLoadExec: executable path");
    path[static_cast<std::size_t>(size)] = '\0';
    return path.data();
#endif
}

void ResetSignals() {
    sigset_t none;
    sigemptyset(&none);
    if (pthread_sigmask(SIG_SETMASK, &none, nullptr) != 0) throw std::runtime_error("sceSystemServiceLoadExec: cannot unblock signals");
    for (int number = 1; number < NSIG; ++number) {
        struct sigaction action {};
        if (sigaction(number, nullptr, &action) != 0 || action.sa_handler != SIG_IGN) continue;
        action.sa_handler = SIG_DFL;
        sigaction(number, &action, nullptr);
    }
}

void CloseDescriptorsOnExec() {
#ifdef __linux__
    std::vector<int> descriptors;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        const auto descriptor = std::stoi(entry.path().filename().string());
        if (descriptor > STDERR_FILENO) descriptors.push_back(descriptor);
    }
    for (const auto descriptor : descriptors) {
        const auto flags = fcntl(descriptor, F_GETFD);
        if (flags >= 0) fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
    }
#endif
}

}

[[noreturn]] void SystemService::ReplaceProcess(const char* path, const char* const* arguments) {
    bool restart = false;
    const auto resolved = ResolveExecutable(path, restart);
    const auto executable = restart ? CurrentExecutablePath() : resolved.string();
    const auto copiedArguments = CopyArguments(arguments);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : copiedArguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    LibcRunShutdown_nid_postfix();
    std::fflush(nullptr);
    CloseDescriptorsOnExec();
    ResetSignals();
    execv(executable.c_str(), argv.data());
    throw std::system_error(errno, std::generic_category(), "sceSystemServiceLoadExec: starting " + std::string(path));
}

#endif
