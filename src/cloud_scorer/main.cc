// mozkey_cloud_scorer: a ZNZ1 named-pipe server that corrects Mozkey live
// conversion with an OpenAI-compatible cloud chat completion API (for example
// OpenCode Go).  It speaks the same protocol as mozc_zenz_scorer, so
// mozc_server only switches the pipe name.  When the cloud request fails or no
// API key is configured, the request is answered with the local Zenz scorer
// result instead.
//
// Settings (all optional except the API key):
//   %LOCALAPPDATA%\Mozkey\cloud_scorer.ini
//     [cloud]
//     endpoint=https://opencode.ai/inference/go/openai/v1/chat/completions
//     model=deepseek-v4.1-flash
//     max_tokens=128
//     extra_body="thinking":{"type":"disabled"}   (default for deepseek-*)
//     local_fallback=1
//   API key: Windows Credential Manager generic credential
//     "Mozkey/CloudScorer" (password field), or the MOZKEY_CLOUD_API_KEY /
//     OPENCODE_API_KEY environment variables.

#define WIN32_LEAN_AND_MEAN

// clang-format off
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <wincred.h>
#include <winhttp.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <future>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cloud_scorer/cloud_prompt.h"
#include "zenz/zenz_wire_protocol.h"

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Winhttp.lib")

namespace {

using ::mozc::zenz::kZenzWireKindRequest;
using ::mozc::zenz::kZenzWireKindResponse;
using ::mozc::zenz::kZenzWireMagic;
using ::mozc::zenz::kZenzWireStatusError;
using ::mozc::zenz::kZenzWireStatusOk;
using ::mozc::zenz::kZenzWireStatusTimeout;
using ::mozc::zenz::kZenzWireVersion;
using ::mozc::zenz::ZenzWireRequestHeader;
using ::mozc::zenz::ZenzWireResponseHeader;

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\mozkey_cloud_scorer";
constexpr wchar_t kLocalPipeName[] = L"\\\\.\\pipe\\mozc_zenz_scorer";
constexpr wchar_t kLocalScorerExe[] = L"mozc_zenz_scorer.exe";
constexpr wchar_t kSingleInstanceMutexName[] =
    L"Local\\MozkeyCloudScorerSingleInstance";
// Development/test instances set MOZKEY_CLOUD_SCORER_INSTANCE=<[A-Za-z0-9_]+>
// to serve "<kPipeName>_<instance>" next to an installed scorer.
constexpr wchar_t kInstanceEnv[] = L"MOZKEY_CLOUD_SCORER_INSTANCE";
constexpr wchar_t kCredentialTarget[] = L"Mozkey/CloudScorer";

constexpr wchar_t kDefaultEndpoint[] =
    L"https://opencode.ai/inference/go/openai/v1/chat/completions";
constexpr wchar_t kDefaultModel[] = L"deepseek-v4.1-flash";
constexpr int kDefaultMaxTokens = 128;
// Marks extra_body as absent from the ini (an empty value is a valid choice).
constexpr wchar_t kExtraBodyUnset[] = L"\x01";
// DeepSeek V4 thinks by default, which takes 2-3 s per request; disabled it
// answers in about 1.2 s warm.  Applied only when extra_body is unset.
constexpr char kDeepSeekDefaultExtraBody[] = "\"thinking\":{\"type\":\"disabled\"}";
constexpr int kMaxMaxTokens = 1024;

constexpr uint32_t kMaxPromptBytes = 8192;
constexpr uint32_t kMaxOutputChars = 256;
constexpr uint32_t kMaxRequestTimeoutMsec = 8000;
constexpr uint32_t kMinRequestTimeoutMsec = 50;
// The local scorer is only a fallback; never let it hold the response longer
// than its own normal budget.
constexpr uint32_t kMaxLocalFallbackTimeoutMsec = 1000;
constexpr uint32_t kPipeConnectPollMsec = 1000;
constexpr uint32_t kPipeIoSafetyMsec = kMaxRequestTimeoutMsec + 2000;
constexpr size_t kMaxHttpResponseBytes = 256 * 1024;
constexpr uint32_t kMaxResponsePayloadBytes = 64 * 1024;
constexpr DWORD kLocalLaunchThrottleMsec = 2000;

enum class IoResult { kOk, kTimeout, kError };

bool DebugEnabled() {
  static const bool enabled = [] {
    wchar_t buf[8] = {};
    return ::GetEnvironmentVariableW(L"MOZKEY_CLOUD_SCORER_DEBUG", buf, 8) >
               0 &&
           buf[0] == L'1';
  }();
  return enabled;
}

// Never log user text; only lengths and reasons.
void Debug(const std::wstring& message) {
  if (!DebugEnabled()) return;
  ::OutputDebugStringW((L"[mozkey_cloud_scorer] " + message + L"\n").c_str());
}

std::wstring Utf8ToWide(const std::string& input) {
  if (input.empty()) return std::wstring();
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, input.data(),
                                         static_cast<int>(input.size()),
                                         nullptr, 0);
  if (size <= 0) return std::wstring();
  std::wstring out(size, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, input.data(),
                        static_cast<int>(input.size()), out.data(), size);
  return out;
}

std::string WideToUtf8(const std::wstring& input) {
  if (input.empty()) return std::string();
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, input.data(),
                                         static_cast<int>(input.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (size <= 0) return std::string();
  std::string out(size, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, input.data(),
                        static_cast<int>(input.size()), out.data(), size,
                        nullptr, nullptr);
  return out;
}

std::wstring GetEnvWide(const wchar_t* name) {
  const DWORD size = ::GetEnvironmentVariableW(name, nullptr, 0);
  if (size == 0) return std::wstring();
  std::wstring value(size, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(name, value.data(), size);
  value.resize(written);
  return value;
}

std::wstring GetExeDirectory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD size = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (size == 0 || size >= MAX_PATH) return std::wstring();
  std::wstring full(path, size);
  const size_t pos = full.find_last_of(L"\\/");
  return pos == std::wstring::npos ? std::wstring() : full.substr(0, pos);
}

std::wstring KnownFolderPath(REFKNOWNFOLDERID id) {
  PWSTR folder = nullptr;
  std::wstring path;
  if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &folder))) {
    path = folder;
  }
  ::CoTaskMemFree(folder);
  return path;
}

struct SettingsLocation {
  std::wstring path;
  // Short ASCII tag reported in the response debug string ("cfg=...").  When
  // nothing is found it lists each candidate's Win32 error, e.g.
  // "none(env:5,known:5,low:2)".
  std::string source = "none";
};

// mozc_server starts this process inside its sandbox (low integrity,
// restricted token, locked-down job), where the settings file under
// %LOCALAPPDATA% was observed to be unreadable.  Try the inherited
// LOCALAPPDATA, the known folder, then LocalLow (writable from low integrity),
// and use the first file that exists.
SettingsLocation GetSettingsLocation() {
  constexpr wchar_t kRelative[] = L"\\Mozkey\\cloud_scorer.ini";
  const std::pair<std::wstring, const char*> candidates[] = {
      {GetEnvWide(L"LOCALAPPDATA"), "env"},
      {KnownFolderPath(FOLDERID_LocalAppData), "known"},
      {KnownFolderPath(FOLDERID_LocalAppDataLow), "low"},
  };
  std::string errors;
  for (const auto& [dir, source] : candidates) {
    if (!errors.empty()) errors += ",";
    errors += source;
    if (dir.empty()) {
      errors += ":nodir";
      continue;
    }
    const std::wstring path = dir + kRelative;
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES &&
        (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return {path, source};
    }
    errors += ":" + std::to_string(::GetLastError());
  }
  return {std::wstring(), "none(" + errors + ")"};
}

std::wstring ReadIniString(const std::wstring& path, const wchar_t* key,
                           const wchar_t* default_value) {
  if (path.empty()) return default_value;
  std::vector<wchar_t> buffer(4096);
  const DWORD len = ::GetPrivateProfileStringW(
      L"cloud", key, default_value, buffer.data(),
      static_cast<DWORD>(buffer.size()), path.c_str());
  return std::wstring(buffer.data(), len);
}

// Reads the API key from MOZKEY_CLOUD_API_KEY (an explicit per-process
// override), Windows Credential Manager, then OPENCODE_API_KEY.
std::string LoadApiKey() {
  const std::wstring override_key = GetEnvWide(L"MOZKEY_CLOUD_API_KEY");
  if (!override_key.empty()) return WideToUtf8(override_key);
  PCREDENTIALW credential = nullptr;
  if (::CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential)) {
    std::string key;
    const DWORD size = credential->CredentialBlobSize;
    if (credential->CredentialBlob != nullptr && size > 0) {
      // cmdkey and the Credential Manager UI store the password as UTF-16.
      if (size % sizeof(wchar_t) == 0) {
        key = WideToUtf8(std::wstring(
            reinterpret_cast<const wchar_t*>(credential->CredentialBlob),
            size / sizeof(wchar_t)));
      } else {
        key.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                   size);
      }
    }
    ::SecureZeroMemory(credential->CredentialBlob, size);
    ::CredFree(credential);
    if (!key.empty()) return key;
  }
  return WideToUtf8(GetEnvWide(L"OPENCODE_API_KEY"));
}

struct Options {
  std::wstring endpoint;
  std::string model;
  int max_tokens = kDefaultMaxTokens;
  std::string extra_body_members;
  bool local_fallback = true;
  std::string api_key;
  std::string settings_source = "none";
};

// Settings are re-read for each request so edits apply without restarting.
Options LoadOptions() {
  const SettingsLocation location = GetSettingsLocation();
  const std::wstring& ini = location.path;
  Options options;
  options.settings_source = location.source;
  options.endpoint = ReadIniString(ini, L"endpoint", kDefaultEndpoint);
  options.model = WideToUtf8(ReadIniString(ini, L"model", kDefaultModel));
  const std::wstring max_tokens = ReadIniString(ini, L"max_tokens", L"");
  if (!max_tokens.empty()) {
    const int parsed = _wtoi(max_tokens.c_str());
    if (parsed > 0) options.max_tokens = std::min(parsed, kMaxMaxTokens);
  }
  options.extra_body_members =
      WideToUtf8(ReadIniString(ini, L"extra_body", kExtraBodyUnset));
  options.local_fallback = ReadIniString(ini, L"local_fallback", L"1") != L"0";
  const std::wstring env_endpoint = GetEnvWide(L"MOZKEY_CLOUD_ENDPOINT");
  if (!env_endpoint.empty()) options.endpoint = env_endpoint;
  const std::wstring env_model = GetEnvWide(L"MOZKEY_CLOUD_MODEL");
  if (!env_model.empty()) options.model = WideToUtf8(env_model);
  const std::wstring env_extra = GetEnvWide(L"MOZKEY_CLOUD_EXTRA_BODY");
  if (!env_extra.empty()) options.extra_body_members = WideToUtf8(env_extra);
  const std::wstring env_fallback = GetEnvWide(L"MOZKEY_CLOUD_LOCAL_FALLBACK");
  if (!env_fallback.empty()) options.local_fallback = env_fallback != L"0";
  if (options.extra_body_members == "\x01") {
    options.extra_body_members.clear();
    if (options.model.rfind("deepseek", 0) == 0) {
      options.extra_body_members = kDeepSeekDefaultExtraBody;
    }
  }
  options.api_key = LoadApiKey();
  return options;
}

// Returns "_<instance>" for a valid MOZKEY_CLOUD_SCORER_INSTANCE, else "".
std::wstring GetInstanceSuffix() {
  const std::wstring instance = GetEnvWide(kInstanceEnv);
  if (instance.empty() || instance.size() > 32) return std::wstring();
  for (const wchar_t c : instance) {
    if (!((L'a' <= c && c <= L'z') || (L'A' <= c && c <= L'Z') ||
          (L'0' <= c && c <= L'9') || c == L'_')) {
      return std::wstring();
    }
  }
  return L"_" + instance;
}

DWORD RemainingMsec(Deadline deadline) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - Clock::now());
  if (remaining.count() <= 0) return 0;
  return static_cast<DWORD>(
      std::min<int64_t>(remaining.count(), std::numeric_limits<DWORD>::max()));
}

class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE handle = nullptr) : handle_(handle) {}
  ~ScopedHandle() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
    }
  }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  HANDLE get() const { return handle_; }

 private:
  HANDLE handle_;
};

// ---------------------------------------------------------------------------
// Overlapped pipe I/O with absolute deadlines.

IoResult FinishOverlapped(HANDLE handle, OVERLAPPED* overlapped,
                          Deadline deadline, DWORD* transferred) {
  const DWORD wait =
      ::WaitForSingleObject(overlapped->hEvent, RemainingMsec(deadline));
  if (wait == WAIT_OBJECT_0) {
    return ::GetOverlappedResult(handle, overlapped, transferred, FALSE)
               ? IoResult::kOk
               : IoResult::kError;
  }
  ::CancelIoEx(handle, overlapped);
  DWORD ignored = 0;
  ::GetOverlappedResult(handle, overlapped, &ignored, TRUE);
  return wait == WAIT_TIMEOUT ? IoResult::kTimeout : IoResult::kError;
}

IoResult TransferAll(HANDLE handle, void* data, uint32_t size, bool write,
                     Deadline deadline) {
  uint8_t* ptr = static_cast<uint8_t*>(data);
  uint32_t remaining = size;
  while (remaining > 0) {
    ScopedHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (event.get() == nullptr) return IoResult::kError;
    OVERLAPPED overlapped = {};
    overlapped.hEvent = event.get();
    const BOOL started =
        write ? ::WriteFile(handle, ptr, remaining, nullptr, &overlapped)
              : ::ReadFile(handle, ptr, remaining, nullptr, &overlapped);
    if (!started && ::GetLastError() != ERROR_IO_PENDING) {
      return IoResult::kError;
    }
    DWORD transferred = 0;
    const IoResult result =
        FinishOverlapped(handle, &overlapped, deadline, &transferred);
    if (result != IoResult::kOk) return result;
    if (transferred == 0) return IoResult::kError;
    ptr += transferred;
    remaining -= transferred;
  }
  return IoResult::kOk;
}

IoResult ReadAll(HANDLE h, void* data, uint32_t size, Deadline deadline) {
  return TransferAll(h, data, size, /*write=*/false, deadline);
}

IoResult WriteAll(HANDLE h, const void* data, uint32_t size,
                  Deadline deadline) {
  return TransferAll(h, const_cast<void*>(data), size, /*write=*/true,
                     deadline);
}

IoResult ConnectPipeUntil(HANDLE pipe, Deadline deadline) {
  ScopedHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (event.get() == nullptr) return IoResult::kError;
  OVERLAPPED overlapped = {};
  overlapped.hEvent = event.get();
  if (::ConnectNamedPipe(pipe, &overlapped)) return IoResult::kOk;
  const DWORD error = ::GetLastError();
  if (error == ERROR_PIPE_CONNECTED) return IoResult::kOk;
  if (error != ERROR_IO_PENDING) return IoResult::kError;
  DWORD ignored = 0;
  return FinishOverlapped(pipe, &overlapped, deadline, &ignored);
}

// ---------------------------------------------------------------------------
// Local Zenz scorer fallback.

struct BackendResult {
  uint32_t status = kZenzWireStatusError;
  std::string value;
  std::string debug;
};

void LaunchLocalScorerIfNeeded() {
  static std::atomic<DWORD> last_launch{0};
  const DWORD now = ::GetTickCount();
  const DWORD previous = last_launch.load();
  if (previous != 0 && now - previous < kLocalLaunchThrottleMsec) return;
  last_launch = now;

  const std::wstring dir = GetExeDirectory();
  const std::wstring exe = dir + L"\\" + kLocalScorerExe;
  if (::GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    Debug(L"local scorer not found");
    return;
  }
  std::wstring command_line = L"\"" + exe + L"\"";
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, dir.c_str(), &startup,
                       &process)) {
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
  }
}

HANDLE OpenLocalPipe(Deadline deadline) {
  bool launched = false;
  while (RemainingMsec(deadline) > 0) {
    HANDLE pipe = ::CreateFileW(
        kLocalPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
        nullptr);
    if (pipe != INVALID_HANDLE_VALUE) return pipe;
    const DWORD error = ::GetLastError();
    if (error == ERROR_PIPE_BUSY) {
      ::WaitNamedPipeW(kLocalPipeName,
                       std::min<DWORD>(RemainingMsec(deadline), 50));
      continue;
    }
    if ((error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) &&
        !launched) {
      LaunchLocalScorerIfNeeded();
      launched = true;
    }
    ::Sleep(std::min<DWORD>(RemainingMsec(deadline), 20));
  }
  return INVALID_HANDLE_VALUE;
}

BackendResult QueryLocalScorer(ZenzWireRequestHeader header,
                               const std::string& prompt,
                               uint32_t timeout_msec) {
  BackendResult result;
  const Deadline deadline =
      Clock::now() + std::chrono::milliseconds(timeout_msec);
  ScopedHandle pipe(OpenLocalPipe(deadline));
  if (pipe.get() == INVALID_HANDLE_VALUE) {
    result.status = kZenzWireStatusTimeout;
    result.debug = "local_pipe_open_failed";
    return result;
  }
  header.timeout_msec = timeout_msec;
  ZenzWireResponseHeader response = {};
  if (WriteAll(pipe.get(), &header, sizeof(header), deadline) !=
          IoResult::kOk ||
      WriteAll(pipe.get(), prompt.data(), static_cast<uint32_t>(prompt.size()),
               deadline) != IoResult::kOk ||
      ReadAll(pipe.get(), &response, sizeof(response), deadline) !=
          IoResult::kOk) {
    result.status = kZenzWireStatusTimeout;
    result.debug = "local_pipe_io_failed";
    return result;
  }
  if (response.magic != kZenzWireMagic ||
      response.version != kZenzWireVersion ||
      response.kind != kZenzWireKindResponse ||
      response.generation != header.generation ||
      response.value_size > kMaxResponsePayloadBytes ||
      response.debug_size > kMaxResponsePayloadBytes) {
    result.debug = "local_bad_response";
    return result;
  }
  std::string value(response.value_size, '\0');
  std::string debug(response.debug_size, '\0');
  if ((response.value_size > 0 &&
       ReadAll(pipe.get(), value.data(), response.value_size, deadline) !=
           IoResult::kOk) ||
      (response.debug_size > 0 &&
       ReadAll(pipe.get(), debug.data(), response.debug_size, deadline) !=
           IoResult::kOk)) {
    result.status = kZenzWireStatusTimeout;
    result.debug = "local_payload_io_failed";
    return result;
  }
  result.status = response.status;
  result.value = std::move(value);
  result.debug = "local_" + debug;
  return result;
}

// ---------------------------------------------------------------------------
// Cloud chat completion over WinHTTP.  The session and connection handles are
// kept between requests so TLS connections are reused (keep-alive), which
// matters more for IME latency than anything else here.

class CloudClient {
 public:
  CloudClient() : session_id_(NewSessionId()) {}
  ~CloudClient() { Reset(); }

  BackendResult Complete(const Options& options,
                         const mozc::cloud_scorer::ZenzPromptFields& fields,
                         uint32_t timeout_msec, uint32_t max_output_chars) {
    BackendResult result;
    if (options.api_key.empty()) {
      result.debug = "cloud_no_api_key";
      return result;
    }

    URL_COMPONENTS url = {};
    url.dwStructSize = sizeof(url);
    url.dwHostNameLength = static_cast<DWORD>(-1);
    url.dwUrlPathLength = static_cast<DWORD>(-1);
    url.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(options.endpoint.c_str(), 0, 0, &url)) {
      result.debug = "cloud_bad_endpoint";
      return result;
    }
    const std::wstring host(url.lpszHostName, url.dwHostNameLength);
    std::wstring path(url.lpszUrlPath, url.dwUrlPathLength);
    path.append(url.lpszExtraInfo, url.dwExtraInfoLength);
    const bool secure = url.nScheme == INTERNET_SCHEME_HTTPS;
    // Never send the API key in clear text to a remote host.
    if (!secure && host != L"localhost" && host != L"127.0.0.1") {
      result.debug = "cloud_endpoint_not_https";
      return result;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureConnection(host, url.nPort)) {
      result.debug = "cloud_connect_failed";
      return result;
    }

    mozc::cloud_scorer::ChatRequestOptions request_options;
    request_options.model = options.model;
    request_options.max_tokens = options.max_tokens;
    request_options.extra_body_members = options.extra_body_members;
    const std::string body =
        mozc::cloud_scorer::BuildChatCompletionsBody(fields, request_options);

    HINTERNET request = ::WinHttpOpenRequest(
        connect_, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr) {
      result.debug = "cloud_open_request_failed";
      return result;
    }
    const int timeout = static_cast<int>(timeout_msec);
    ::WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);

    // OpenCode Go rejects requests without a stable per-conversation
    // x-opencode-session ID (HTTP 400).  One IME process is treated as one
    // conversation; other OpenAI-compatible services ignore the header.
    std::wstring headers =
        L"Content-Type: application/json; charset=utf-8\r\n"
        L"Accept: application/json\r\n"
        L"x-opencode-session: " +
        session_id_ +
        L"\r\n"
        L"Authorization: Bearer " +
        Utf8ToWide(options.api_key) + L"\r\n";

    // WinHTTP timeouts are per phase, so a slow response can overrun the
    // request budget several times.  A watchdog closes the request handle at
    // the deadline, which aborts the blocked synchronous call; otherwise the
    // single-instance pipe would stay busy and miss the next live request.
    std::mutex watchdog_mutex;
    std::condition_variable watchdog_cv;
    bool finished = false;
    bool aborted = false;
    std::thread watchdog([&] {
      std::unique_lock<std::mutex> watchdog_lock(watchdog_mutex);
      if (!watchdog_cv.wait_for(watchdog_lock,
                                std::chrono::milliseconds(timeout_msec),
                                [&] { return finished; })) {
        aborted = true;
        ::WinHttpCloseHandle(request);
      }
    });

    std::string response_body;
    DWORD status_code = 0;
    // |phase| names the last step started; it is reported on failure so a
    // stall inside mozc_server's sandbox can be located without a debugger.
    const ULONGLONG http_start = ::GetTickCount64();
    const char* phase = "send";
    bool ok = ::WinHttpSendRequest(request, headers.c_str(),
                                   static_cast<DWORD>(-1L),
                                   const_cast<char*>(body.data()),
                                   static_cast<DWORD>(body.size()),
                                   static_cast<DWORD>(body.size()), 0);
    if (ok) {
      phase = "receive";
      ok = ::WinHttpReceiveResponse(request, nullptr);
    }
    if (ok) {
      phase = "status";
      ok = QueryStatus(request, &status_code);
    }
    if (ok) {
      phase = "body";
      ok = ReadBody(request, &response_body);
    }
    const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
    const std::string phase_info =
        std::string("@") + phase + "_" +
        std::to_string(::GetTickCount64() - http_start) + "ms";
    {
      std::lock_guard<std::mutex> watchdog_lock(watchdog_mutex);
      finished = true;
      if (!aborted) ::WinHttpCloseHandle(request);
    }
    watchdog_cv.notify_all();
    watchdog.join();
    ::SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));

    if (aborted || !ok) {
      // Drop the cached connection so the next request starts clean.
      Reset();
      const bool timed_out = aborted || error == ERROR_WINHTTP_TIMEOUT;
      result.status = timed_out ? kZenzWireStatusTimeout : kZenzWireStatusError;
      result.debug = (timed_out ? std::string("cloud_timeout")
                                : "cloud_http_failed_" + std::to_string(error)) +
                     phase_info + "_err" + std::to_string(error);
      return result;
    }
    if (status_code != 200) {
      result.debug = "cloud_http_status_" + std::to_string(status_code);
      const std::string message =
          mozc::cloud_scorer::ExtractApiErrorMessage(response_body, 160);
      if (!message.empty()) result.debug += ":" + message;
      return result;
    }

    std::string text;
    std::string parse_error;
    if (!mozc::cloud_scorer::ExtractChatCompletionText(response_body, &text,
                                                       &parse_error)) {
      result.debug = "cloud_" + parse_error;
      return result;
    }
    text = mozc::cloud_scorer::CleanModelOutput(text, max_output_chars);
    if (text.empty()) {
      result.debug = "cloud_empty_output";
      return result;
    }
    result.status = kZenzWireStatusOk;
    result.value = std::move(text);
    result.debug = "cloud_ok" + phase_info;
    return result;
  }

 private:
  bool EnsureConnection(const std::wstring& host, INTERNET_PORT port) {
    if (connect_ != nullptr && host == host_ && port == port_) return true;
    Reset();
    session_ = ::WinHttpOpen(L"mozkey_cloud_scorer/1.0",
                             WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_ == nullptr) {
      session_ = ::WinHttpOpen(L"mozkey_cloud_scorer/1.0",
                               WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                               0);
    }
    if (session_ == nullptr) return false;
    connect_ = ::WinHttpConnect(session_, host.c_str(), port, 0);
    if (connect_ == nullptr) {
      Reset();
      return false;
    }
    host_ = host;
    port_ = port;
    return true;
  }

  void Reset() {
    if (connect_ != nullptr) ::WinHttpCloseHandle(connect_);
    if (session_ != nullptr) ::WinHttpCloseHandle(session_);
    connect_ = nullptr;
    session_ = nullptr;
    host_.clear();
    port_ = 0;
  }

  static bool QueryStatus(HINTERNET request, DWORD* status_code) {
    DWORD size = sizeof(*status_code);
    return ::WinHttpQueryHeaders(
        request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, status_code, &size,
        WINHTTP_NO_HEADER_INDEX);
  }

  static bool ReadBody(HINTERNET request, std::string* body) {
    while (true) {
      DWORD available = 0;
      if (!::WinHttpQueryDataAvailable(request, &available)) return false;
      if (available == 0) return true;
      if (body->size() + available > kMaxHttpResponseBytes) {
        ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return false;
      }
      const size_t offset = body->size();
      body->resize(offset + available);
      DWORD read = 0;
      if (!::WinHttpReadData(request, body->data() + offset, available,
                             &read)) {
        return false;
      }
      body->resize(offset + read);
    }
  }

  static std::wstring NewSessionId() {
    GUID guid = {};
    wchar_t text[64] = {};
    if (FAILED(::CoCreateGuid(&guid)) ||
        ::StringFromGUID2(guid, text, 64) == 0) {
      return L"mozkey-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
             std::to_wstring(::GetTickCount64());
    }
    std::wstring id = L"mozkey-";
    for (const wchar_t* p = text; *p != L'\0'; ++p) {
      if (*p != L'{' && *p != L'}') id.push_back(::towlower(*p));
    }
    return id;
  }

  const std::wstring session_id_;
  std::mutex mutex_;
  HINTERNET session_ = nullptr;
  HINTERNET connect_ = nullptr;
  std::wstring host_;
  INTERNET_PORT port_ = 0;
};

// ---------------------------------------------------------------------------
// Pipe server.

bool BuildCurrentUserOnlySecurityAttributes(PSECURITY_DESCRIPTOR* sd,
                                            SECURITY_ATTRIBUTES* sa) {
  *sd = nullptr;
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  DWORD size = 0;
  ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<uint8_t> buffer(size);
  auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
  LPWSTR sid = nullptr;
  const bool got_sid =
      size > 0 &&
      ::GetTokenInformation(token, TokenUser, user, size, &size) &&
      ::ConvertSidToStringSidW(user->User.Sid, &sid);
  ::CloseHandle(token);
  if (!got_sid) return false;
  // Protected DACL: current user read/write, SYSTEM full.  No Everyone access.
  const std::wstring sddl =
      std::wstring(L"D:P(A;;GRGW;;;") + sid + L")(A;;GA;;;SY)";
  ::LocalFree(sid);
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, sd, nullptr)) {
    *sd = nullptr;
    return false;
  }
  *sa = {};
  sa->nLength = sizeof(*sa);
  sa->lpSecurityDescriptor = *sd;
  sa->bInheritHandle = FALSE;
  return true;
}

void SendResponse(HANDLE pipe, uint32_t generation, const BackendResult& result,
                  uint32_t latency_msec, Deadline deadline) {
  ZenzWireResponseHeader header = {};
  header.magic = kZenzWireMagic;
  header.version = kZenzWireVersion;
  header.kind = kZenzWireKindResponse;
  header.generation = generation;
  header.status = result.status;
  header.latency_msec = latency_msec;
  header.value_size = static_cast<uint32_t>(result.value.size());
  header.debug_size = static_cast<uint32_t>(result.debug.size());
  if (WriteAll(pipe, &header, sizeof(header), deadline) != IoResult::kOk) {
    return;
  }
  if (!result.value.empty()) {
    WriteAll(pipe, result.value.data(),
             static_cast<uint32_t>(result.value.size()), deadline);
  }
  if (!result.debug.empty()) {
    WriteAll(pipe, result.debug.data(),
             static_cast<uint32_t>(result.debug.size()), deadline);
  }
}

BackendResult ErrorResult(const char* reason) {
  BackendResult result;
  result.debug = reason;
  return result;
}

void HandleClient(HANDLE pipe, CloudClient* cloud) {
  const DWORD start = ::GetTickCount();
  const Deadline io_deadline =
      Clock::now() + std::chrono::milliseconds(kPipeIoSafetyMsec);

  ZenzWireRequestHeader header = {};
  if (ReadAll(pipe, &header, sizeof(header), io_deadline) != IoResult::kOk) {
    return;
  }
  if (header.magic != kZenzWireMagic || header.version != kZenzWireVersion ||
      header.kind != kZenzWireKindRequest) {
    SendResponse(pipe, header.generation, ErrorResult("bad_request_header"), 0,
                 io_deadline);
    return;
  }
  if (header.prompt_size == 0 || header.prompt_size > kMaxPromptBytes) {
    SendResponse(pipe, header.generation, ErrorResult("bad_prompt_size"), 0,
                 io_deadline);
    return;
  }
  std::string prompt(header.prompt_size, '\0');
  if (ReadAll(pipe, prompt.data(), header.prompt_size, io_deadline) !=
      IoResult::kOk) {
    return;
  }

  const uint32_t timeout_msec = std::clamp<uint32_t>(
      header.timeout_msec == 0 ? kMaxRequestTimeoutMsec : header.timeout_msec,
      kMinRequestTimeoutMsec, kMaxRequestTimeoutMsec);
  const uint32_t max_output_chars = std::clamp<uint32_t>(
      header.max_output_chars == 0 ? kMaxOutputChars : header.max_output_chars,
      1, kMaxOutputChars);

  mozc::cloud_scorer::ZenzPromptFields fields;
  if (!mozc::cloud_scorer::ParseZenzPrompt(prompt, &fields)) {
    SendResponse(pipe, header.generation, ErrorResult("unparsable_prompt"), 0,
                 io_deadline);
    return;
  }

  const Options options = LoadOptions();

  // Run the local scorer concurrently so a cloud failure costs no extra time.
  std::future<BackendResult> local;
  if (options.local_fallback) {
    local = std::async(std::launch::async, QueryLocalScorer, header,
                       mozc::cloud_scorer::StripCloudOnlySegments(prompt),
                       std::min(timeout_msec, kMaxLocalFallbackTimeoutMsec));
  }

  BackendResult result =
      cloud->Complete(options, fields, timeout_msec, max_output_chars);
  Debug(L"cloud " + Utf8ToWide(result.debug) + L" value_bytes=" +
        std::to_wstring(result.value.size()));

  if (result.status != kZenzWireStatusOk && local.valid()) {
    BackendResult fallback = local.get();
    fallback.debug = result.debug + ";" + fallback.debug;
    result = std::move(fallback);
  }
  // Which settings file was used ("none" = built-in defaults).
  result.debug += std::string(";cfg=") + options.settings_source;

  SendResponse(pipe, header.generation, result, ::GetTickCount() - start,
               io_deadline);
  // |local| (if still running) is joined here by the future destructor; it is
  // bounded by kMaxLocalFallbackTimeoutMsec.
}

int RunServer() {
  CloudClient cloud;
  const std::wstring pipe_name = kPipeName + GetInstanceSuffix();
  while (true) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    if (!BuildCurrentUserOnlySecurityAttributes(&sd, &sa)) {
      Debug(L"security descriptor failed");
      ::Sleep(1000);
      continue;
    }
    HANDLE pipe = ::CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, 65536, 65536, 0, &sa);
    ::LocalFree(sd);
    if (pipe == INVALID_HANDLE_VALUE) {
      Debug(L"CreateNamedPipeW failed error=" +
            std::to_wstring(::GetLastError()));
      ::Sleep(1000);
      continue;
    }
    const IoResult connected = ConnectPipeUntil(
        pipe, Clock::now() + std::chrono::milliseconds(kPipeConnectPollMsec));
    if (connected == IoResult::kOk) {
      HandleClient(pipe, &cloud);
      ::FlushFileBuffers(pipe);
      ::DisconnectNamedPipe(pipe);
    }
    ::CloseHandle(pipe);
  }
}

}  // namespace

int wmain() {
  HANDLE single_instance =
      ::CreateMutexW(nullptr, TRUE,
                     (kSingleInstanceMutexName + GetInstanceSuffix()).c_str());
  if (single_instance != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS) {
    ::CloseHandle(single_instance);
    return 0;
  }
  return RunServer();
}
