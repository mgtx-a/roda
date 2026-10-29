// The job queue: each job runs in its own worker process (Roda.exe --worker), at most half as many
// at once as the PC has cores. Workers sit in a Job Object, so they end with Roda, even after a crash.
#pragma once
#include "registry.h"

namespace rd {

struct JobState {
    int id = 0;
    wstring title;            // "photo.png"
    wstring what;             // "WEBP", "Compress"...
    enum Phase { Queued, Running, Done, Failed, Cancelled } phase = Queued;
    double progress = 0;
    wstring out, error, part;
    uint64_t before = 0, after = 0;
    Json info;                // extra results (e.g. the picture size, for previews)
    bool quiet = false;       // no card: a preview for Float or a popup
    DWORD helper = 0;         // Office started by this job's worker: ended too when the job is cancelled
};

class Jobs {
public:
    Jobs();
    ~Jobs();
    /// Queues a job; returns its id. `title` and `what` are for the card.
    int Submit(const Job& job, const wstring& title, const wstring& what, bool quiet = false);
    void Cancel(int id);
    void CancelAll();
    const JobState* Find(int id) const;

    std::function<void(const JobState&)> onChange;   // on the UI thread

private:
    struct Running;
    void Pump();
    bool Start(int id);
    void OnLine(int id, const std::string& line);
    void OnExit(int id, DWORD code);
    void RemovePart(const wstring& part, int attempt);

    HANDLE jobObject_ = nullptr;
    int nextId_ = 1;
    int maxRunning_ = 1;
    std::vector<JobState> states_;
    std::vector<std::pair<int, Job>> queue_;
    std::vector<std::unique_ptr<Running>> running_;
};

}  // namespace rd
