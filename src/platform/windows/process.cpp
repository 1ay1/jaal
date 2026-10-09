// windows_process — the Windows half of jaal/platform/process.hpp.
// The only file that sees <windows.h> for this backend.

#include <jaal/platform/windows/process.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace jaal::platform {

namespace {

error win_error(const char* what) {
    return error{std::errc::io_error, static_cast<std::int32_t>(::GetLastError()), what};
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// CommandLineToArgvW quoting: a run of backslashes doubles before a quote,
// a quote is escaped, and an argument is wrapped only when it needs it.
void append_quoted(std::wstring& out, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        out += arg;
        return;
    }
    out.push_back(L'"');
    std::size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
}

// Windows sorts environment names case-insensitively; so does the block.
struct ci_less {
    bool operator()(const std::wstring& a, const std::wstring& b) const noexcept {
        return ::_wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

// The environment block for CreateProcessW, or empty to inherit unchanged.
std::vector<wchar_t> env_block(const process_spec& spec) {
    if (spec.env.empty() && !spec.env_is_complete) return {};
    std::map<std::wstring, std::wstring, ci_less> vars;
    if (!spec.env_is_complete) {
        if (wchar_t* base = ::GetEnvironmentStringsW()) {
            for (const wchar_t* p = base; *p; p += std::wcslen(p) + 1) {
                std::wstring_view kv{p};
                // Entries like "=C:=C:\" (per-drive cwd) start with '='.
                const auto eq = kv.find(L'=', 1);
                if (eq == std::wstring_view::npos) continue;
                vars[std::wstring{kv.substr(0, eq)}] = std::wstring{kv.substr(eq + 1)};
            }
            ::FreeEnvironmentStringsW(base);
        }
    }
    for (const auto& [k, v] : spec.env) vars[widen(k)] = widen(v);
    std::vector<wchar_t> out;
    for (const auto& [k, v] : vars) {
        out.insert(out.end(), k.begin(), k.end());
        out.push_back(L'=');
        out.insert(out.end(), v.begin(), v.end());
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    if (out.size() == 1) out.push_back(L'\0');   // an empty block is two NULs
    return out;
}

struct pipe_pair {
    owned_handle r, w;
};

// Both ends inheritable; the caller clears inheritance on the parent's end.
result<pipe_pair> make_pipe() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE r = nullptr, w = nullptr;
    // 64 KiB: a chatty child fills the 4 KiB default in one printf and then
    // stalls on our drain.
    if (!::CreatePipe(&r, &w, &sa, 64 * 1024)) return std::unexpected(win_error("CreatePipe"));
    return pipe_pair{owned_handle{r}, owned_handle{w}};
}

result<owned_handle> open_nul(bool write) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = ::CreateFileW(L"NUL", write ? GENERIC_WRITE : GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::unexpected(win_error("CreateFile(NUL)"));
    return owned_handle{h};
}

void not_inherited(const owned_handle& h) {
    if (h) ::SetHandleInformation(static_cast<HANDLE>(h.get()), HANDLE_FLAG_INHERIT, 0);
}

}  // namespace

struct windows_process::impl {
    owned_handle  process;
    owned_handle  job;
    owned_handle  out_r, err_r, in_w;
    DWORD         pid    = 0;
    bool          reaped = false;
};

windows_process::windows_process() : p_(std::make_unique<impl>()) {}
windows_process::windows_process(windows_process&&) noexcept            = default;
windows_process& windows_process::operator=(windows_process&&) noexcept = default;

windows_process::~windows_process() {
    if (!p_ || p_->reaped || !p_->process) return;
    // Still running and nobody reaped it: take the tree down rather than
    // leak it. KILL_ON_JOB_CLOSE would do it when the job handle closes;
    // terminating first makes it happen before we return.
    if (p_->job) ::TerminateJobObject(static_cast<HANDLE>(p_->job.get()), 1);
}

result<windows_process> windows_process::spawn(const process_spec& spec) {
    if (spec.argv.empty())
        return std::unexpected(error::make(std::errc::invalid_argument,
                                           "windows_process: empty argv"));
    windows_process self;
    auto& s = *self.p_;

    // ── standard streams ──────────────────────────────────────────────
    owned_handle child_in, child_out, child_err;

    switch (spec.stdin_from) {
        case stream_to::pipe: {
            auto p = make_pipe();
            if (!p) return std::unexpected(p.error());
            child_in = std::move(p->r);
            s.in_w   = std::move(p->w);
            not_inherited(s.in_w);
            break;
        }
        case stream_to::null: {
            auto n = open_nul(false);
            if (!n) return std::unexpected(n.error());
            child_in = std::move(*n);
            break;
        }
        case stream_to::inherit: break;
    }

    const bool merge = spec.merge_stderr && spec.stdout_to == stream_to::pipe
                    && spec.stderr_to == stream_to::pipe;

    auto setup_out = [&](stream_to to, owned_handle& child, owned_handle& parent)
        -> result<void> {
        switch (to) {
            case stream_to::pipe: {
                auto p = make_pipe();
                if (!p) return std::unexpected(p.error());
                child  = std::move(p->w);
                parent = std::move(p->r);
                not_inherited(parent);
                return {};
            }
            case stream_to::null: {
                auto n = open_nul(true);
                if (!n) return std::unexpected(n.error());
                child = std::move(*n);
                return {};
            }
            case stream_to::inherit: return {};
        }
        return {};
    };
    if (auto r = setup_out(spec.stdout_to, child_out, s.out_r); !r)
        return std::unexpected(r.error());
    if (!merge) {
        if (auto r = setup_out(spec.stderr_to, child_err, s.err_r); !r)
            return std::unexpected(r.error());
    }

    STARTUPINFOW si{};
    si.cb      = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = child_in  ? static_cast<HANDLE>(child_in.get())
                              : ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = child_out ? static_cast<HANDLE>(child_out.get())
                              : ::GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = merge     ? si.hStdOutput
                  : child_err ? static_cast<HANDLE>(child_err.get())
                              : ::GetStdHandle(STD_ERROR_HANDLE);

    // ── the tree: a job that dies with its last handle ────────────────
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (!job) return std::unexpected(win_error("CreateJobObject"));
    s.job = owned_handle{job};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));

    // ── command line, environment, directory ──────────────────────────
    std::wstring cmd;
    if (!spec.windows_command_line.empty()) {
        cmd = widen(spec.windows_command_line);
    } else {
        for (std::size_t i = 0; i < spec.argv.size(); ++i) {
            if (i) cmd.push_back(L' ');
            append_quoted(cmd, widen(spec.argv[i]));
        }
    }
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');
    std::vector<wchar_t> env = env_block(spec);
    const std::wstring cwd = widen(spec.cwd);

    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    // new_session: no console of ours to read or to receive our Ctrl-C.
    if (spec.new_session) flags |= CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;

    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, flags,
                                     env.empty() ? nullptr : env.data(),
                                     cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    // The child holds its own copies now. Closing ours is what lets the
    // parent see EOF when the child ends.
    child_in.reset();
    child_out.reset();
    child_err.reset();
    if (!ok) return std::unexpected(win_error("CreateProcessW"));

    s.process = owned_handle{pi.hProcess};
    s.pid     = pi.dwProcessId;
    // Into the job BEFORE it runs, so nothing it starts can be outside.
    if (!::AssignProcessToJobObject(job, pi.hProcess)) {
        const auto e = win_error("AssignProcessToJobObject");
        ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hThread);
        s.reaped = true;
        return std::unexpected(e);
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    return self;
}

borrowed_handle windows_process::exit_handle() const noexcept {
    return borrowed_handle{p_->process.get()};
}

std::optional<borrowed_handle> windows_process::stdout_handle() const {
    if (!p_->out_r) return std::nullopt;
    return borrowed_handle{p_->out_r.get()};
}

std::optional<borrowed_handle> windows_process::stderr_handle() const {
    if (!p_->err_r) return std::nullopt;
    return borrowed_handle{p_->err_r.get()};
}

std::optional<borrowed_handle> windows_process::stdin_handle() const {
    if (!p_->in_w) return std::nullopt;
    return borrowed_handle{p_->in_w.get()};
}

void windows_process::close_stdin() noexcept { p_->in_w.reset(); }

result<void> windows_process::stop(stop_mode, stop_scope scope) {
    if (!p_->process || p_->reaped) return {};
    const HANDLE proc = static_cast<HANDLE>(p_->process.get());
    if (::WaitForSingleObject(proc, 0) == WAIT_OBJECT_0 && scope == stop_scope::leader)
        return {};   // already ended
    // Exit code 1 for a killed child; reap() reports it as signalled.
    constexpr UINT kKilled = 0x4B494C4C;   // "KILL"
    if (scope == stop_scope::tree && p_->job) {
        if (!::TerminateJobObject(static_cast<HANDLE>(p_->job.get()), kKilled))
            return std::unexpected(win_error("TerminateJobObject"));
        return {};
    }
    if (!::TerminateProcess(proc, kKilled)) {
        // Lost the race with the exit: not an error.
        if (::WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) return {};
        return std::unexpected(win_error("TerminateProcess"));
    }
    return {};
}

result<exit_status> windows_process::reap() {
    if (p_->reaped)
        return std::unexpected(error::make(std::errc::no_child_process,
                                           "windows_process: already reaped"));
    const HANDLE proc = static_cast<HANDLE>(p_->process.get());
    if (::WaitForSingleObject(proc, 0) != WAIT_OBJECT_0)
        return std::unexpected(error::make(std::errc::operation_would_block,
                                           "windows_process: still running"));
    DWORD code = 0;
    if (!::GetExitCodeProcess(proc, &code)) return std::unexpected(win_error("GetExitCodeProcess"));
    p_->reaped = true;
    exit_status st;
    if (code == 0x4B494C4C) {
        st.how  = exit_status::kind::signalled;
        st.code = 9;   // what a POSIX caller would see for SIGKILL
    } else {
        st.how  = exit_status::kind::exited;
        st.code = static_cast<std::int32_t>(code);
    }
    return st;
}

std::uint64_t windows_process::id() const noexcept { return p_->pid; }

std::size_t read_some(borrowed_handle h, char* buf, std::size_t cap, bool& eof) noexcept {
    eof = false;
    if (!h || cap == 0) return 0;
    const HANDLE ph = static_cast<HANDLE>(h.get());
    DWORD avail = 0;
    if (!::PeekNamedPipe(ph, nullptr, 0, nullptr, &avail, nullptr)) {
        eof = true;   // the writer closed: EOF
        return 0;
    }
    if (avail == 0) return 0;
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(cap, avail));
    if (!::ReadFile(ph, buf, want, &got, nullptr)) {
        eof = true;
        return 0;
    }
    return got;
}

std::size_t write_some(borrowed_handle h, const char* buf, std::size_t len,
                       bool& closed) noexcept {
    closed = false;
    if (!h || len == 0) return 0;
    // Anonymous pipes have no non-blocking write; cap a write to what the
    // 64 KiB buffer takes so a full pipe stalls us one chunk at most.
    DWORD put = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(len, 4096));
    if (!::WriteFile(static_cast<HANDLE>(h.get()), buf, want, &put, nullptr)) {
        closed = true;
        return 0;
    }
    return put;
}

}  // namespace jaal::platform
