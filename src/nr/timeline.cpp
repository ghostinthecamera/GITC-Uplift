#include "nr/timeline.hpp"

#include <algorithm>
#include <format>
#include <mutex>
#include <utility>
#include <vector>

#include "nr/log.hpp"

namespace uplift::nr {
namespace {

// A submission its own thread has not stamped yet is stamped by any present once it is this old
// and this many frames behind: its ExecuteCommandLists has returned long before (amendment 1).
constexpr auto AGED_SUBMISSION_TIME = std::chrono::milliseconds(250);
constexpr uint64_t AGED_SUBMISSION_FRAMES = 2u;

// A release waits on every token issued before it, so a token stuck this long (never submitted,
// dropped or stamped through to completion) would hold VRAM silently: the stutter Plan 1 fixed
// would come back undiagnosed. One WARN per Timeline (one per device, in practice one per process).
constexpr auto STUCK_TOKEN_AGE = std::chrono::seconds(5);

// One fence per queue, created on first use. The device outlives its Timeline (DeviceContext).
class D3D12QueueFences final : public QueueFences {
 public:
  explicit D3D12QueueFences(ID3D12Device* device) : device_(device) {}
  ~D3D12QueueFences() override {
    if (event_ != nullptr) {
      CloseHandle(event_);
    }
  }
  D3D12QueueFences(const D3D12QueueFences&) = delete;
  D3D12QueueFences& operator=(const D3D12QueueFences&) = delete;

  bool Signal(ID3D12CommandQueue* queue, uint64_t value) override {
    auto& fence = fences_[queue];
    if (!fence && FAILED(device_->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
      fences_.erase(queue);
      Log(LogLevel::WARN, "could not create a completion-token fence");
      return false;
    }
    const HRESULT result = queue->Signal(fence.Get(), value);
    if (FAILED(result)) {
      Logf(LogLevel::WARN, "completion-token Signal failed with {:#010x}", static_cast<uint32_t>(result));
      return false;
    }
    return true;
  }
  [[nodiscard]] uint64_t Completed(ID3D12CommandQueue* queue) const override {
    const auto found = fences_.find(queue);
    return (found == fences_.end() ? 0u : found->second->GetCompletedValue());
  }
  bool Wait(ID3D12CommandQueue* queue, uint64_t value, std::chrono::milliseconds timeout) override {
    const auto found = fences_.find(queue);
    if (found == fences_.end() || found->second->GetCompletedValue() >= value) return true;
    if (event_ == nullptr) {
      event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (event_ == nullptr || FAILED(found->second->SetEventOnCompletion(value, event_))) return false;
    return WaitForSingleObject(event_, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
  }

 private:
  ID3D12Device* device_;
  std::map<ID3D12CommandQueue*, Microsoft::WRL::ComPtr<ID3D12Fence>> fences_;
  HANDLE event_ = nullptr;
};

}  // namespace

std::unique_ptr<Timeline> Timeline::CreateD3D12(ID3D12Device* device) {
  Microsoft::WRL::ComPtr<ID3D12Fence> fence;
  if (device == nullptr || FAILED(device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return nullptr;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (event == nullptr) return nullptr;
  ID3D12Fence* raw = fence.Get();
  auto timeline = std::make_unique<Timeline>(
      [raw] { return raw->GetCompletedValue(); },
      [raw](ID3D12CommandQueue* queue, uint64_t value) {
        HRESULT hr = queue->Signal(raw, value);
        if (FAILED(hr)) {
          Log(LogLevel::WARN, std::format("ID3D12CommandQueue::Signal failed with 0x{:08x}", static_cast<uint32_t>(hr)));
        }
      },
      [raw, event](uint64_t value, std::chrono::milliseconds timeout) {
        if (raw->GetCompletedValue() >= value) return true;
        if (FAILED(raw->SetEventOnCompletion(value, event))) return false;
        return WaitForSingleObject(event, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
      });
  timeline->fence_ = std::move(fence);
  timeline->event_ = event;
  timeline->SetQueueFences(std::make_unique<D3D12QueueFences>(device));
  return timeline;
}

Timeline::Timeline(CompletedFn completed, SignalFn signal, WaitFn wait)
    : completed_(std::move(completed)), signal_(std::move(signal)), wait_(std::move(wait)) {}

Timeline::Timeline(Timeline&& other) noexcept
    : completed_(std::move(other.completed_)),
      signal_(std::move(other.signal_)),
      wait_(std::move(other.wait_)),
      last_queue_(other.last_queue_),
      current_frame_(other.current_frame_.load()),
      signaled_(other.signaled_),
      pending_(std::move(other.pending_)),
      fence_(std::move(other.fence_)),
      event_(std::exchange(other.event_, nullptr)),
      tokens_(std::move(other.tokens_)),
      last_token_(other.last_token_),
      unstamped_(other.unstamped_.load()),
      fences_(std::move(other.fences_)),
      queue_values_(std::move(other.queue_values_)),
      switch_frame_(other.switch_frame_),
      switch_barrier_(other.switch_barrier_),
      reexecutions_(std::move(other.reexecutions_)),
      stuck_warned_(other.stuck_warned_.load()),
      token_probe_(std::move(other.token_probe_)) {}

Timeline::~Timeline() {
  if (event_ != nullptr) {
    CloseHandle(event_);
  }
  if (!pending_.empty()) {
    Log(LogLevel::WARN, std::format("Timeline destroyed with {} pending releases; call Flush before destroying", pending_.size()));
  }
}

void Timeline::SetQueueFences(std::unique_ptr<QueueFences> fences) {
  const std::unique_lock lock(tokens_mutex_);
  fences_ = std::move(fences);
}

void Timeline::SetTokenProbe(std::function<bool(uint64_t token)> probe) {
  const std::unique_lock lock(tokens_mutex_);
  token_probe_ = std::move(probe);
}

uint64_t Timeline::BeginFrame(ID3D12CommandQueue* queue) {
  const uint64_t current = current_frame_.load(std::memory_order_acquire);
  if (queue != nullptr && last_queue_ != nullptr && queue != last_queue_) {
    // Frame values signalled on the new present queue do not follow the old queue's work, so every
    // mark taken at or before this frame also waits for one barrier on the old queue (Plan 2 final
    // review note). Unavailable, same as before Task 5, when there is no fence backend at all.
    {
      const std::unique_lock lock(tokens_mutex_);
      if (fences_ != nullptr) {
        const uint64_t value = ++queue_values_[last_queue_];
        const uint64_t barrier = ++last_token_;
        // Important 1 (fix round 1): every mark taken at or before this frame now also needs
        // `barrier` complete, folded in dynamically by IsComplete(const Mark&) -- not just the
        // marks already sitting in pending_ at this exact moment (a mark committed to pending_
        // later, such as NrPipeline's on a deferred release, would otherwise miss this entirely).
        switch_frame_ = current;
        switch_barrier_ = barrier;
        if (fences_->Signal(last_queue_, value)) {
          tokens_.emplace(barrier, Token{.state = Token::State::STAMPED, .queue = last_queue_, .issue_time = std::chrono::steady_clock::now(), .submit_frame = CurrentFrame(), .fence_value = value});
        } else {
          // The barrier Signal failed: rather than silently leaving every pending release
          // unprotected (Task 5 review), keep the token SUBMITTED and tag it with this, the
          // presenting thread, so its own next reset, evaluate or present -- a StampThread call --
          // retries the same Signal, exactly like any other failed stamp (StampLocked below).
          // P7: left at its default (the epoch), submit_time also makes StampAged treat this token as
          // aged the moment its frame count passes -- an extra, thread-independent way to retry the
          // same Signal from any present, on top of this thread's own StampThread call. Both paths
          // only ever retry Signal, so this is safe.
          tokens_.emplace(barrier, Token{.state = Token::State::SUBMITTED, .queue = last_queue_, .thread_id = GetCurrentThreadId(), .issue_time = std::chrono::steady_clock::now(), .submit_frame = CurrentFrame()});
          unstamped_.fetch_add(1u);
        }
      }
    }
    Log(LogLevel::INFO, "the present queue changed; pending releases also wait for the previous queue");
  }
  if (current > 0u && queue != nullptr) {
    signal_(queue, current);
    signaled_ = current;
  }
  if (queue != nullptr) {
    last_queue_ = queue;
  }
  return current_frame_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
}

bool Timeline::IsComplete(uint64_t frame) const {
  return frame == 0u || completed_() >= frame;
}

bool Timeline::IsComplete(const Mark& mark) const {
  if (mark.frame == 0u && mark.token == 0u) return true;
  uint64_t token = mark.token;
  {
    const std::shared_lock lock(tokens_mutex_);
    if (mark.frame != 0u && mark.frame <= switch_frame_) {
      token = std::max(token, switch_barrier_);  // Plan 3, Important 1
    }
    for (const Reexecution& reexecution : reexecutions_) {
      if (mark.token >= reexecution.first_token) {
        token = std::max(token, reexecution.fresh_token);  // N12
      }
    }
  }
  return IsComplete(mark.frame) && TokensComplete(token);
}

Mark Timeline::MarkNow() const {
  return {.frame = CurrentFrame(), .token = LastToken()};
}

void Timeline::ReleaseAfter(uint64_t frame, std::function<void()> release) {
  ReleaseAfter(Mark{.frame = frame, .token = LastToken()}, std::move(release));
}

void Timeline::ReleaseAfter(const Mark& mark, std::function<void()> release) {
  pending_.push_back({mark, std::move(release)});
}

void Timeline::Poll() {
  {
    const std::unique_lock lock(tokens_mutex_);
    if (fences_ != nullptr) {
      std::erase_if(tokens_, [this](const auto& entry) {
        const Token& token = entry.second;
        return token.state == Token::State::STAMPED && fences_->Completed(token.queue) >= token.fence_value;
      });
    }
    if (token_probe_) {
      // Plan 13: the tokens the probe calls complete (their VkEvent is set) are done, whatever their state.
      std::erase_if(tokens_, [this](const auto& entry) {
        if (!token_probe_(entry.first)) return false;
        if (entry.second.state == Token::State::SUBMITTED) {
          unstamped_.fetch_sub(1u);
        }
        return true;
      });
    }
    std::erase_if(reexecutions_, [this](const Reexecution& reexecution) { return !tokens_.contains(reexecution.fresh_token); });
  }
  if (pending_.empty()) return;
  std::deque<Pending> ready;
  std::deque<Pending> remaining;
  for (auto& entry : pending_) {
    if (IsComplete(entry.mark)) {
      ready.push_back(std::move(entry));
    } else {
      remaining.push_back(std::move(entry));
    }
  }
  pending_ = std::move(remaining);
  for (auto& entry : ready) {
    entry.release();
  }
}

std::string_view Timeline::WaitIdle(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string_view late;  // the first wait that ran out
  const uint64_t current = CurrentFrame();
  if (last_queue_ != nullptr && current > signaled_) {
    signal_(last_queue_, current);
    signaled_ = current;
  }
  if (current > 0u && !wait_(current, timeout)) {
    late = "Timeline flush timed out";
  }
  bool probed = false;
  {
    const std::shared_lock lock(tokens_mutex_);
    probed = static_cast<bool>(token_probe_);
  }
  if (probed) {
    // Plan 13: a host cannot wait on a VkEvent (only the GPU does, and only the host polls), so the tokens are polled until the deadline.
    const uint64_t newest = LastToken();
    while (!TokensComplete(newest) && std::chrono::steady_clock::now() < deadline) {
      Sleep(1u);
    }
    if (!TokensComplete(newest) && late.empty()) {
      late = "a completion-token event did not complete in time";
    }
  }
  const std::unique_lock lock(tokens_mutex_);
  // The device-destroy flush stamps every submitted token unconditionally: every
  // ExecuteCommandLists has returned by then, so any thread's tokens are safe to stamp. That
  // holds at device destruction only. A present-queue teardown while the device still lives
  // (Task 10) would also need to stamp everything here, but a game thread caught between its
  // execute_command_list event and its native call at that moment is not protected by this --
  // an accepted, rare gap (spec amendment 1). Plan 15: the game's own NGX shutdown is the same case.
  StampLocked([](const Token& /*token*/) { return true; });
  if (fences_ != nullptr) {
    for (const auto& [queue, value] : queue_values_) {
      const auto left = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
                                 std::chrono::milliseconds(0));
      if (!fences_->Wait(queue, value, left)) {
        if (late.empty()) {
          late = "a completion-token wait timed out";
        }
        break;
      }
    }
  }
  return late;
}

void Timeline::Flush(std::chrono::milliseconds timeout) {
  if (const std::string_view late = WaitIdle(timeout); !late.empty()) {
    Logf(LogLevel::WARN, "{}; releasing anyway at device teardown", late);
  }
  {
    const std::unique_lock lock(tokens_mutex_);
    tokens_.clear();
    reexecutions_.clear();
    unstamped_.store(0u);
  }
  while (!pending_.empty()) {
    std::deque<Pending> batch = std::move(pending_);
    for (auto& entry : batch) {
      entry.release();
    }
  }
}

uint64_t Timeline::IssueToken(std::chrono::steady_clock::time_point now) {
  const std::unique_lock lock(tokens_mutex_);
  const uint64_t id = ++last_token_;
  tokens_.emplace(id, Token{.issue_time = now});
  return id;
}

void Timeline::Submit(std::span<const uint64_t> tokens, ID3D12CommandQueue* queue, uint32_t thread_id,
                      std::chrono::steady_clock::time_point now) {
  const std::unique_lock lock(tokens_mutex_);
  for (const uint64_t id : tokens) {
    const auto found = tokens_.find(id);
    if (found == tokens_.end() || found->second.state != Token::State::RECORDED) continue;
    found->second = {
        .state = Token::State::SUBMITTED,
        .queue = queue,
        .thread_id = thread_id,
        .issue_time = found->second.issue_time,  // preserve: this replaces the whole Token
        .submit_frame = CurrentFrame(),
        .submit_time = now,
    };
    unstamped_.fetch_add(1u);
  }
}

uint64_t Timeline::Resubmit(uint64_t first_token, ID3D12CommandQueue* queue, uint32_t thread_id,
                            std::chrono::steady_clock::time_point now) {
  const std::unique_lock lock(tokens_mutex_);
  const uint64_t fresh = ++last_token_;
  if (token_probe_) {
    // Plan 13: the probe decides when this execution is done; nothing stamps it, so it stays RECORDED (never counted as unstamped).
    tokens_.emplace(fresh, Token{.queue = queue, .thread_id = thread_id, .issue_time = now, .submit_frame = CurrentFrame(), .submit_time = now});
  } else {
    tokens_.emplace(fresh, Token{.state = Token::State::SUBMITTED, .queue = queue, .thread_id = thread_id, .issue_time = now, .submit_frame = CurrentFrame(), .submit_time = now});
    unstamped_.fetch_add(1u);
  }
  reexecutions_.push_back({.first_token = first_token, .fresh_token = fresh});
  return fresh;
}

void Timeline::Drop(std::span<const uint64_t> tokens) {
  const std::unique_lock lock(tokens_mutex_);
  for (const uint64_t id : tokens) {
    const auto found = tokens_.find(id);
    if (found != tokens_.end() && found->second.state == Token::State::RECORDED) {
      tokens_.erase(found);
    }
  }
}

void Timeline::StampThread(uint32_t thread_id) {
  if (unstamped_.load(std::memory_order_relaxed) == 0u) return;
  const std::unique_lock lock(tokens_mutex_);
  StampLocked([thread_id](const Token& token) { return token.thread_id == thread_id; });
}

void Timeline::StampAged(std::chrono::steady_clock::time_point now) {
  if (!stuck_warned_.load(std::memory_order_relaxed)) {
    const std::shared_lock lock(tokens_mutex_);
    for (const auto& [id, token] : tokens_) {
      // tokens_ is keyed by issue order, so the first entry not yet STAMPED is the oldest
      // outstanding one: a STAMPED token with a smaller id is just waiting on its fence, not stuck.
      if (token.state == Token::State::STAMPED) continue;
      // Plan 13: a token the probe already calls complete is waiting for Poll to prune it, not stuck.
      if (token_probe_ && token_probe_(id)) continue;
      // P8: exchange, not a load then a store -- today's outer relaxed load above already makes a
      // second WARN merely unlikely (every caller holds the add-on lock in practice), and this makes
      // it impossible even if that ever stopped holding: only the caller that flips the latch logs.
      if (now - token.issue_time > STUCK_TOKEN_AGE && !stuck_warned_.exchange(true, std::memory_order_relaxed)) {
        Logf(LogLevel::WARN, "a completion token has been outstanding for over {}s; NR memory may be stuck",
             std::chrono::duration_cast<std::chrono::seconds>(now - token.issue_time).count());
      }
      break;
    }
  }
  if (unstamped_.load(std::memory_order_relaxed) == 0u) return;
  const std::unique_lock lock(tokens_mutex_);
  const uint64_t frame = CurrentFrame();
  StampLocked([now, frame](const Token& token) {
    return now - token.submit_time >= AGED_SUBMISSION_TIME && frame >= token.submit_frame + AGED_SUBMISSION_FRAMES;
  });
}

void Timeline::ForgetQueue(ID3D12CommandQueue* queue) {
  const std::unique_lock lock(tokens_mutex_);
  for (auto it = tokens_.begin(); it != tokens_.end();) {
    if (it->second.queue != queue || it->second.state == Token::State::RECORDED) {
      ++it;
      continue;
    }
    if (it->second.state == Token::State::SUBMITTED) {
      unstamped_.fetch_sub(1u);
    }
    it = tokens_.erase(it);
  }
}

size_t Timeline::OutstandingTokens() const {
  const std::shared_lock lock(tokens_mutex_);
  return tokens_.size();
}

uint64_t Timeline::LastToken() const {
  const std::shared_lock lock(tokens_mutex_);
  return last_token_;
}

bool Timeline::TokensComplete(uint64_t through) const {
  if (through == 0u) return true;
  const std::shared_lock lock(tokens_mutex_);
  for (const auto& [id, token] : tokens_) {
    if (id > through) break;
    if (token_probe_ && token_probe_(id)) continue;  // Plan 13: a Vulkan token is complete when its event says so, whatever its state
    if (token.state != Token::State::STAMPED || fences_ == nullptr || fences_->Completed(token.queue) < token.fence_value) {
      return false;
    }
  }
  return true;
}

void Timeline::StampLocked(const std::function<bool(const Token&)>& matches) {
  if (fences_ == nullptr) return;
  std::vector<ID3D12CommandQueue*> queues;
  for (const auto& [id, token] : tokens_) {
    if (token.state == Token::State::SUBMITTED && matches(token) && std::ranges::find(queues, token.queue) == queues.end()) {
      queues.push_back(token.queue);
    }
  }
  for (ID3D12CommandQueue* const queue : queues) {
    const uint64_t value = ++queue_values_[queue];
    if (!fences_->Signal(queue, value)) continue;  // stays SUBMITTED: a later stamp or Flush retries
    for (auto& [id, token] : tokens_) {
      if (token.state == Token::State::SUBMITTED && token.queue == queue && matches(token)) {
        token.state = Token::State::STAMPED;
        token.fence_value = value;
        unstamped_.fetch_sub(1u);
      }
    }
  }
}

}  // namespace uplift::nr
