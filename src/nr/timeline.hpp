#pragma once

#include <Windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string_view>
#include <vector>

namespace uplift::nr {

// A point in GPU progress (v2 design §3.3): a present-queue frame, plus every completion token
// issued before the mark was taken. It is complete once both are.
struct Mark {
  uint64_t frame = 0u;
  uint64_t token = 0u;  // the newest token issued when the mark was taken; 0 = none
};

// The per-queue fences completion tokens are stamped with. Timeline::CreateD3D12 installs one fence
// per queue, created on first use; unit tests inject fakes. Signal, Wait and SetQueueFences run
// under the Timeline's token lock held exclusively, so none of them ever overlaps another call.
// Completed, though, is called under that lock's shared mode: several threads can call it at once
// (every thread-safe token query does), so an implementation must tolerate concurrent calls to
// Completed, even though it is never concurrent with a Signal or a Wait.
class QueueFences {
 public:
  virtual ~QueueFences() = default;
  // Signals `queue`'s fence to `value` on `queue`. False when the signal could not be issued.
  virtual bool Signal(ID3D12CommandQueue* queue, uint64_t value) = 0;
  // The value `queue`'s fence has reached; 0 for a queue never signalled.
  [[nodiscard]] virtual uint64_t Completed(ID3D12CommandQueue* queue) const = 0;
  // Device teardown only: waits up to `timeout` for `queue`'s fence to reach `value`.
  virtual bool Wait(ID3D12CommandQueue* queue, uint64_t value, std::chrono::milliseconds timeout) = 0;
};

// Frame-numbered GPU progress. Frames start at 1. BeginFrame(frame n+1)
// signals n on the queue, so IsComplete(n) means everything submitted for
// frame n, including work flushed at its present, has finished.
//
// Completion tokens cover Uplift's recordings on command lists the game submits, on any queue
// (v2 design §3.3 and this plan's amendment 1). A token is issued when Uplift records, submitted
// with its list, stamped by a Signal on that queue's own fence from the thread that submitted it
// (or, after 250 ms and two frames, from any present), and complete once that fence passes the
// stamp. The token methods are thread-safe; the frame methods run on one thread at a time.
class Timeline {
 public:
  using CompletedFn = std::function<uint64_t()>;
  using SignalFn = std::function<void(ID3D12CommandQueue*, uint64_t)>;
  using WaitFn = std::function<bool(uint64_t value, std::chrono::milliseconds timeout)>;

  static std::unique_ptr<Timeline> CreateD3D12(ID3D12Device* device);
  Timeline(CompletedFn completed, SignalFn signal, WaitFn wait);
  ~Timeline();
  Timeline(Timeline&& other) noexcept;
  Timeline& operator=(Timeline&&) = delete;
  Timeline(const Timeline&) = delete;
  Timeline& operator=(const Timeline&) = delete;

  // Installs the fences tokens are stamped with. Tokens stay outstanding until fences exist.
  void SetQueueFences(std::unique_ptr<QueueFences> fences);
  // Plan 13 (design §3.3): Vulkan's tokens are VkEvents set at the end of each hooked evaluate. With a probe, TokensComplete asks it for
  // every outstanding token, whatever its state; Poll prunes the tokens it says are complete; a teardown Flush polls it until the deadline
  // (a host cannot wait on an event); Resubmit's fresh token stays RECORDED (nothing stamps it). The D3D12 path never sets one and behaves
  // exactly as before. The probe is called under the token lock (shared by TokensComplete, exclusive by Poll): it must be thread-safe, and
  // it must never call back into this Timeline.
  void SetTokenProbe(std::function<bool(uint64_t token)> probe);

  uint64_t BeginFrame(ID3D12CommandQueue* queue);
  [[nodiscard]] uint64_t CurrentFrame() const { return current_frame_.load(std::memory_order_acquire); }
  [[nodiscard]] bool IsComplete(uint64_t frame) const;
  // Fix round 1, Important 1: a mark taken at or before the latest present-queue switch's frame also
  // needs that switch's barrier token complete, whatever `mark.token` itself is -- the switch means
  // the shared frame fence no longer proves the old queue's work is done. Token completion is
  // cumulative, so requiring only the latest switch's barrier also covers every earlier one.
  [[nodiscard]] bool IsComplete(const Mark& mark) const;
  // The current frame and the newest token issued so far.
  [[nodiscard]] Mark MarkNow() const;
  // Waits for `frame` and for every token issued before this call.
  void ReleaseAfter(uint64_t frame, std::function<void()> release);
  void ReleaseAfter(const Mark& mark, std::function<void()> release);
  // Runs releases whose mark completed. Never blocks.
  void Poll();
  // Device teardown only: signal the current frame on the last queue, stamp every submitted token,
  // wait for all of it within `timeout` in total, then run every pending release regardless.
  void Flush(std::chrono::milliseconds timeout);
  // Plan 15: Flush's wait alone (the frame signal, every submitted token stamped, the waits within `timeout` in total), with nothing released and no
  // token forgotten. Empty when everything finished in time; otherwise what did not ("Timeline flush timed out", ...). The same stamping caveat as Flush:
  // only while no game thread is between its execute_command_list event and its native call (the game's own NGX shutdown, at the device's teardown).
  [[nodiscard]] std::string_view WaitIdle(std::chrono::milliseconds timeout);
  [[nodiscard]] size_t PendingCount() const { return pending_.size(); }

  // A new token, for a recording Uplift is about to make on a game command list. `now` defaults to
  // the real clock; a test wanting a deterministic issue time for the stuck-token check (StampAged)
  // passes its own.
  [[nodiscard]] uint64_t IssueToken(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  // `tokens`' list was submitted to `queue` (native) by thread `thread_id`. ReShade raises
  // execute_command_list before the native ExecuteCommandLists, so nothing is signalled yet.
  void Submit(std::span<const uint64_t> tokens, ID3D12CommandQueue* queue, uint32_t thread_id,
              std::chrono::steady_clock::time_point now);
  // N12 (Plan 3's deferred full fix): a closed list whose Uplift work was first recorded with token
  // `first_token` was submitted to `queue` again without a Reset. A fresh token covers this execution,
  // and every mark taken since `first_token` was issued also waits for it: the resources those marks
  // guard may be read again.
  // Returns the fresh token's id (Plan 13: a Vulkan caller maps it to the list's event).
  uint64_t Resubmit(uint64_t first_token, ID3D12CommandQueue* queue, uint32_t thread_id, std::chrono::steady_clock::time_point now);
  // `tokens`' list was reset or destroyed without being submitted: those tokens are complete.
  void Drop(std::span<const uint64_t> tokens);
  // At every Uplift event on thread `thread_id` EXCEPT execute_command_list (reset, evaluate, present):
  // that thread's earlier ExecuteCommandLists calls have returned, so one Signal per queue now follows
  // them. Never from execute_command_list: ReShade raises it for every list of one ExecuteCommandLists
  // call before the single native call, so a Signal there could precede the batch's own submission.
  // Stamps that thread's submitted tokens.
  void StampThread(uint32_t thread_id);
  // At a present, from any thread: stamps submissions still unstamped after 250 ms and two frames.
  void StampAged(std::chrono::steady_clock::time_point now);
  // The game destroyed `queue` (native) after idling it: forget the tokens submitted or stamped on it, so
  // no later stamp or Flush signals a released queue. The queue's fence and last value stay: a new queue at
  // the same address keeps counting up from it.
  void ForgetQueue(ID3D12CommandQueue* queue);
  [[nodiscard]] size_t OutstandingTokens() const;

 private:
  struct Pending {
    Mark mark;
    std::function<void()> release;
  };
  struct Token {
    enum class State : uint8_t {
      RECORDED,
      SUBMITTED,
      STAMPED,
    };
    State state = State::RECORDED;
    ID3D12CommandQueue* queue = nullptr;
    uint32_t thread_id = 0u;
    std::chrono::steady_clock::time_point issue_time;  // set by IssueToken; the stuck-token check's age
    uint64_t submit_frame = 0u;
    std::chrono::steady_clock::time_point submit_time;
    uint64_t fence_value = 0u;
  };

  [[nodiscard]] uint64_t LastToken() const;
  // True when every token with an id up to `through` is complete.
  [[nodiscard]] bool TokensComplete(uint64_t through) const;
  // With the token lock held: one Signal per queue for the SUBMITTED tokens `matches` accepts.
  void StampLocked(const std::function<bool(const Token&)>& matches);

  CompletedFn completed_;
  SignalFn signal_;
  WaitFn wait_;
  ID3D12CommandQueue* last_queue_ = nullptr;
  std::atomic<uint64_t> current_frame_{0u};
  uint64_t signaled_ = 0u;
  std::deque<Pending> pending_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  HANDLE event_ = nullptr;

  mutable std::shared_mutex tokens_mutex_;
  std::map<uint64_t, Token> tokens_;  // outstanding tokens by id
  uint64_t last_token_ = 0u;
  std::atomic<uint32_t> unstamped_{0u};  // SUBMITTED tokens: lets StampThread return without the lock
  std::unique_ptr<QueueFences> fences_;
  std::map<ID3D12CommandQueue*, uint64_t> queue_values_;  // the last value signalled on each queue's fence
  // Important 1: the latest present-queue switch's frame and barrier token, both guarded by
  // tokens_mutex_ like the rest of this group. 0/0 (the initial value) never matches a real mark,
  // since frames start at 1.
  uint64_t switch_frame_ = 0u;
  uint64_t switch_barrier_ = 0u;
  struct Reexecution {
    uint64_t first_token = 0u;  // the list's first Uplift token
    uint64_t fresh_token = 0u;  // this execution's own
  };
  std::vector<Reexecution> reexecutions_;      // guarded by tokens_mutex_; pruned by Poll once each fresh token completes
  std::atomic<bool> stuck_warned_{false};      // one stuck-token WARN per Timeline's lifetime (one per device)
  std::function<bool(uint64_t)> token_probe_;  // Plan 13; guarded by tokens_mutex_; empty on D3D12
};

}  // namespace uplift::nr
