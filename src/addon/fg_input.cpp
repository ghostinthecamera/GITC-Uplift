#include "addon/fg_input.hpp"

#include <algorithm>
#include <format>

#include "text/hudless_messages.hpp"

namespace uplift::addon {
namespace {

// Design §1: HIDE outlives its Present by at most this; USE never does (each Present drops its image first).
constexpr std::chrono::steady_clock::duration NR_IMAGE_LIMIT = std::chrono::seconds(1);
constexpr int64_t NEVER = 0;

constexpr std::string_view NO_HUDLESS_REASON = text::HUDLESS_NO_IMAGE;
constexpr std::string_view NOT_CHANGED_REASON = text::HUDLESS_NOT_CHANGED;
constexpr std::string_view OTHER_FRAME_REASON = text::HUDLESS_OTHER_FRAME;
constexpr std::string_view NOT_USED_REASON = text::HUDLESS_NOT_USED;
constexpr std::string_view HIDE_OFF_REASON = text::HUDLESS_HIDE_OFF;
constexpr std::string_view USED_REASON = text::HUDLESS_USED;
constexpr std::string_view REASONS[] = {NO_HUDLESS_REASON, NOT_CHANGED_REASON, OTHER_FRAME_REASON, NOT_USED_REASON, HIDE_OFF_REASON, USED_REASON};

struct ThreadMark {
  const FgInput* owner = nullptr;
  ImageKey image;
};
thread_local ThreadMark t_mark;

int64_t Ticks(std::chrono::steady_clock::time_point time) {
  const int64_t ticks = time.time_since_epoch().count();
  return (ticks == NEVER ? NEVER + 1 : ticks);
}

bool Within(int64_t stamp, std::chrono::steady_clock::time_point now) {
  return (stamp != NEVER && Ticks(now) - stamp <= NR_IMAGE_LIMIT.count());
}

}  // namespace

void FgInput::CountPresent() noexcept { presents_.fetch_add(1u, std::memory_order_relaxed); }

void FgInput::RefreshReplacementFlagLocked() noexcept {
  const bool any = std::ranges::any_of(entries_, [](const Entry& entry) { return entry.replacement.owner != nullptr; });
  any_replacement_.store(any, std::memory_order_release);
}

void FgInput::AgeLocked(std::vector<std::shared_ptr<const void>>* released) noexcept {
  const uint64_t presents = presents_.load(std::memory_order_relaxed);
  for (Entry& entry : entries_) {
    if (!entry.replacement.owner || presents - entry.noted_present < REPLACEMENT_PRESENTS) continue;
    released->push_back(std::move(entry.replacement.owner));
    entry.replacement = {};
    entry.inputs = {};
  }
}

void FgInput::Drop(ImageKey image, bool counts) noexcept {
  if (counts) CountPresent();
  if (!any_replacement_.load(std::memory_order_acquire)) return;
  std::vector<std::shared_ptr<const void>> released;  // released outside the lock
  const std::lock_guard lock(mutex_);
  AgeLocked(&released);
  for (Entry& entry : entries_) {
    if (entry.image == image) {
      released.push_back(std::move(entry.replacement.owner));
      entry.replacement = {};
      entry.inputs = {};
    }
  }
  RefreshReplacementFlagLocked();
}

void FgInput::DropDevice(uint64_t device) noexcept {
  CountPresent();
  if (!any_replacement_.load(std::memory_order_acquire)) return;
  std::vector<std::shared_ptr<const void>> released;
  const std::lock_guard lock(mutex_);
  AgeLocked(&released);
  for (Entry& entry : entries_) {
    if (entry.image.device == device && entry.replacement.owner) {
      released.push_back(std::move(entry.replacement.owner));
      entry.replacement = {};
      entry.inputs = {};
    }
  }
  RefreshReplacementFlagLocked();
}

void FgInput::DropAll(bool counts) noexcept {
  if (counts) CountPresent();
  if (!any_replacement_.load(std::memory_order_acquire)) return;
  std::vector<std::shared_ptr<const void>> released;
  const std::lock_guard lock(mutex_);
  for (Entry& entry : entries_) {
    if (entry.replacement.owner) released.push_back(std::move(entry.replacement.owner));
    entry.replacement = {};
    entry.inputs = {};
  }
  any_replacement_.store(false, std::memory_order_release);
}

void FgInput::Note(ImageKey image, std::chrono::steady_clock::time_point now, Replacement replacement, HudInputs inputs) {
  std::shared_ptr<const void> released;
  const std::lock_guard lock(mutex_);
  auto found = std::ranges::find(entries_, image, &Entry::image);
  if (found == entries_.end()) {
    entries_.push_back({.image = image});
    found = std::prev(entries_.end());
  }
  released = std::move(found->replacement.owner);
  found->time = now;
  found->replacement = std::move(replacement);
  found->inputs = (found->replacement.owner ? inputs : HudInputs{});
  found->noted_present = presents_.load(std::memory_order_relaxed);
  // Entries older than the limit decide nothing: forget them so the table stays as small as the live images.
  std::erase_if(entries_, [&](const Entry& entry) { return !(entry.image == image) && now - entry.time > NR_IMAGE_LIMIT && !entry.replacement.owner; });
  RefreshReplacementFlagLocked();
  last_note_.store(Ticks(now), std::memory_order_release);
}

FgDecision FgInput::Decide(ImageKey backbuffer, uint64_t game_hudless, uint64_t game_opacity, bool hide, std::chrono::steady_clock::time_point now) {
  if (game_hudless == 0u) return Record({.reason = NO_HUDLESS_REASON}, now);
  if (!AnyRecent(now)) return Record({.reason = NOT_CHANGED_REASON}, now);
  FgDecision decision;
  {
    const std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(entries_, backbuffer, &Entry::image);
    if (found == entries_.end() || now - found->time > NR_IMAGE_LIMIT) {
      decision = {.reason = NOT_CHANGED_REASON};
    } else {
      read_at_.store(Ticks(now), std::memory_order_release);
      if (found->replacement.owner) {
        const bool same_hudless = (found->inputs.hudless == game_hudless);
        // The block may not carry the opacity image; compare it only when both name one.
        const bool same_opacity = (game_opacity == 0u || found->inputs.opacity == 0u || found->inputs.opacity == game_opacity);
        if (same_hudless && same_opacity) {
          decision = {.action = HudlessAction::USE, .replacement = found->replacement, .reason = USED_REASON};
        } else {
          decision = {.action = (hide ? HudlessAction::HIDE : HudlessAction::LEAVE), .reason = OTHER_FRAME_REASON};
        }
      } else {
        decision = (hide ? FgDecision{.action = HudlessAction::HIDE, .reason = NOT_USED_REASON} : FgDecision{.reason = HIDE_OFF_REASON});
      }
    }
  }
  return Record(std::move(decision), now);
}

FgDecision FgInput::Record(FgDecision decision, std::chrono::steady_clock::time_point now) noexcept {
  last_action_.store(static_cast<uint8_t>(decision.action), std::memory_order_relaxed);
  uint8_t index = 0u;
  for (const std::string_view reason : REASONS) {
    if (reason.data() == decision.reason.data()) break;
    ++index;
  }
  last_reason_.store(index, std::memory_order_relaxed);
  last_at_.store(Ticks(now), std::memory_order_release);
  std::atomic<uint64_t>& count = (decision.action == HudlessAction::USE ? used_ : decision.action == HudlessAction::HIDE ? hidden_ : left_);
  count.fetch_add(1u, std::memory_order_relaxed);
  return decision;
}

FgInput::LastDecision FgInput::Last(std::chrono::steady_clock::time_point now) const noexcept {
  if (!Within(last_at_.load(std::memory_order_acquire), now)) return {};
  return {.recent = true, .action = static_cast<HudlessAction>(last_action_.load(std::memory_order_relaxed)),
          .reason = (last_reason_.load(std::memory_order_relaxed) < std::size(REASONS) ? REASONS[last_reason_.load(std::memory_order_relaxed)]
                                                                                      : std::string_view{})};
}

FgInput::Counts FgInput::Totals() const noexcept {
  return {.used = used_.load(std::memory_order_relaxed), .hidden = hidden_.load(std::memory_order_relaxed), .left = left_.load(std::memory_order_relaxed)};
}

std::string HudlessLine(HudlessAction action, std::string_view reason) {
  if (action == HudlessAction::USE) return std::string(text::HUDLESS_USED);
  return std::vformat((action == HudlessAction::HIDE ? text::HUDLESS_HIDDEN : text::HUDLESS_LEFT), std::make_format_args(reason));
}

bool FgInput::FrameGenerationReads(std::chrono::steady_clock::time_point now) const noexcept {
  return Within(read_at_.load(std::memory_order_acquire), now);
}

bool FgInput::AnyRecent(std::chrono::steady_clock::time_point now) const noexcept {
  return Within(last_note_.load(std::memory_order_relaxed), now);
}

uint64_t FgInput::Presents() const noexcept { return presents_.load(std::memory_order_relaxed); }

void FgInput::MarkDroppedOnThisThread(ImageKey image) noexcept { t_mark = {.owner = this, .image = image}; }

bool FgInput::ConsumeThreadMark(ImageKey image) noexcept {
  if (t_mark.owner != this || !(t_mark.image == image)) return false;
  t_mark = {};
  return true;
}

}  // namespace uplift::addon
