#include "jobs.h"

#include <thread>

namespace rd {

struct Jobs::Running {
    int id = 0;
    HANDLE process = nullptr;
    std::thread reader;
    bool cancelled = false;
};

Jobs::Jobs()
{
    // Workers die with Roda: closing the job handle (at exit, or when Roda crashes) ends them all.
    jobObject_ = CreateJobObjectW(nullptr, nullptr);
    if (jobObject_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        SetInformationJobObject(jobObject_, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    maxRunning_ = std::max(1, int(si.dwNumberOfProcessors) / 2);
}

Jobs::~Jobs()
{
    CancelAll();
    for (auto& r : running_) if (r->reader.joinable()) r->reader.join();
    if (jobObject_) CloseHandle(jobObject_);
}

int Jobs::Submit(const Job& job, const wstring& title, const wstring& what, bool quiet)
{
    JobState s;
    s.id = nextId_++;
    s.quiet = quiet;
    s.title = title;
    s.what = what;
    states_.push_back(s);
    queue_.push_back({ s.id, job });
    if (onChange) onChange(states_.back());
    Pump();
    return s.id;
}

const JobState* Jobs::Find(int id) const
{
    for (auto& s : states_) if (s.id == id) return &s;
    return nullptr;
}

void Jobs::Pump()
{
    while (int(running_.size()) < maxRunning_ && !queue_.empty()) {
        int id = queue_.front().first;
        if (!Start(id)) {
            queue_.erase(queue_.begin());
            for (auto& s : states_) if (s.id == id) { s.phase = JobState::Failed; s.error = L"Roda could not start a worker."; if (onChange) onChange(s); }
        }
    }
}

bool Jobs::Start(int id)
{
    Job job = queue_.front().second;
    queue_.erase(queue_.begin());

    // Pipes: the job goes in on stdin, progress comes back on stdout. Only the child's ends are inheritable.
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) return false;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) { CloseHandle(inRead); CloseHandle(inWrite); return false; }
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    wchar_t exe[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    wstring cmd = L"\"" + wstring(exe) + L"\" --worker";
    STARTUPINFOW si{ sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    PROCESS_INFORMATION pi{};
    // Below normal priority: a big conversion never makes the wheel or the cards stutter.
    BOOL ok = CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!ok) { CloseHandle(inWrite); CloseHandle(outRead); Log(L"jobs: could not start a worker (error %lu)", GetLastError()); return false; }
    if (jobObject_) AssignProcessToJobObject(jobObject_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::string text = job.ToJson().Dump();
    DWORD put;
    WriteFile(inWrite, text.data(), DWORD(text.size()), &put, nullptr);
    CloseHandle(inWrite);

    auto r = std::make_unique<Running>();
    r->id = id;
    r->process = pi.hProcess;
    HANDLE proc = pi.hProcess;
    // One thread per worker reads its lines; everything it learns goes to the UI thread.
    r->reader = std::thread([id, outRead, proc, this] {
        std::string buf, line;
        char chunk[4096];
        DWORD got;
        while (ReadFile(outRead, chunk, sizeof chunk, &got, nullptr) && got) {
            buf.append(chunk, got);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                PostToUi([this, id, line] { OnLine(id, line); });
            }
        }
        CloseHandle(outRead);
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(proc, &code);
        PostToUi([this, id, code] { OnExit(id, code); });
    });
    running_.push_back(std::move(r));
    for (auto& s : states_) if (s.id == id) { s.phase = JobState::Running; if (onChange) onChange(s); }
    Log(L"jobs: started %s for %s", job.op.c_str(), job.inputs.size() == 1 ? job.inputs[0].c_str() : (std::to_wstring(job.inputs.size()) + L" items").c_str());
    return true;
}

void Jobs::OnLine(int id, const std::string& line)
{
    Json j;
    if (!Json::Parse(line, j)) return;
    for (auto& s : states_) {
        if (s.id != id) continue;
        if (j.has(L"progress")) s.progress = j[L"progress"].Num();
        if (j.has(L"part")) s.part = j[L"part"].Str();
        if (j.has(L"helper")) s.helper = DWORD(j[L"helper"].Num());
        if (j.has(L"error")) s.error = j[L"error"].Str();
        if (j.has(L"done")) {
            s.out = j[L"done"].Str();
            s.before = uint64_t(j[L"before"].Num());
            s.after = uint64_t(j[L"after"].Num());
            s.info = j;
        }
        if (onChange && s.phase == JobState::Running) onChange(s);
    }
}

void Jobs::RemovePart(const wstring& part, int attempt)
{
    DWORD a = GetFileAttributesW(part.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(part);
    else DeleteFileW(part.c_str());
    if (GetFileAttributesW(part.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (attempt < 10) After(0.5, [this, part, attempt] { RemovePart(part, attempt + 1); });
        else Log(L"jobs: could not remove the partial file %s", part.c_str());
    }
}

void Jobs::OnExit(int id, DWORD code)
{
    for (size_t i = 0; i < running_.size(); i++) {
        if (running_[i]->id != id) continue;
        bool cancelled = running_[i]->cancelled;
        if (running_[i]->reader.joinable()) running_[i]->reader.join();
        CloseHandle(running_[i]->process);
        running_.erase(running_.begin() + long(i));
        for (auto& s : states_) {
            if (s.id != id) continue;
            if (cancelled) {
                s.phase = JobState::Cancelled;
                // The worker was stopped mid-write: its partial output goes (tried again for a few seconds,
                // in case a process that was just ended still holds it).
                if (!s.part.empty()) RemovePart(s.part, 0);
                Log(L"jobs: %s cancelled", s.title.c_str());
            } else if (code == 0 && !s.out.empty()) {
                s.phase = JobState::Done;
                s.progress = 1;
                Log(L"jobs: %s done: %s", s.title.c_str(), s.out.c_str());
            } else {
                s.phase = JobState::Failed;
                if (s.error.empty()) s.error = L"Roda could not convert this file (the worker stopped unexpectedly).";
                Log(L"jobs: %s failed (exit %lu): %s", s.title.c_str(), code, s.error.c_str());
            }
            if (onChange) onChange(s);
        }
        break;
    }
    // Finished jobs are kept a little for the cards, then forgotten.
    if (states_.size() > 200) states_.erase(states_.begin(), states_.begin() + 100);
    Pump();
}

void Jobs::Cancel(int id)
{
    for (auto& q : queue_) {
        if (q.first != id) continue;
        queue_.erase(std::find_if(queue_.begin(), queue_.end(), [id](auto& x) { return x.first == id; }));
        for (auto& s : states_) if (s.id == id) { s.phase = JobState::Cancelled; if (onChange) onChange(s); }
        return;
    }
    for (auto& r : running_) {
        if (r->id == id && !r->cancelled) {
            r->cancelled = true;
            // First the helper (FFmpeg, or a hidden Office) the worker started, waiting until it has let go
            // of the partial file; then the worker, whose end the reader thread reports.
            for (auto& s : states_)
                if (s.id == id && s.helper)
                    if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, s.helper)) {
                        TerminateProcess(h, 1);
                        WaitForSingleObject(h, 3000);
                        CloseHandle(h);
                    }
            TerminateProcess(r->process, 1);
        }
    }
}

void Jobs::CancelAll()
{
    while (!queue_.empty()) Cancel(queue_.front().first);
    for (auto& r : running_) if (!r->cancelled) { r->cancelled = true; TerminateProcess(r->process, 1); }
}

}  // namespace rd
