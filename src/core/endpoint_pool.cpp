#include "mtdl/core/endpoint_pool.hpp"

#include <algorithm>

namespace mtdl {
namespace {
constexpr uint32_t kFailuresToRetire = 2;
}  // namespace

EndpointPool::EndpointPool(std::vector<Endpoint> edges, size_t soft_limit_per_edge)
    : soft_limit_(std::max<size_t>(1, soft_limit_per_edge)),
      last_sample_(std::chrono::steady_clock::now()) {
  edges_.reserve(edges.size());
  for (auto& ep : edges) edges_.push_back(Slot{std::move(ep)});
}

size_t EndpointPool::healthy() const {
  std::lock_guard<std::mutex> lock(mu_);
  size_t n = 0;
  for (const auto& s : edges_) n += s.dead ? 0 : 1;
  return n;
}

int EndpointPool::acquire() {
  std::lock_guard<std::mutex> lock(mu_);

  // Prefer edges below the soft limit: that is what fans connections out over
  // the whole edge set instead of piling them onto one address.
  int best = -1;
  for (size_t i = 0; i < edges_.size(); ++i) {
    const Slot& s = edges_[i];
    if (s.dead || s.active >= soft_limit_) continue;
    if (best < 0 || s.active < edges_[best].active ||
        (s.active == edges_[best].active && s.mibps > edges_[best].mibps))
      best = static_cast<int>(i);
  }
  if (best < 0) {
    // Every edge is at its soft limit: keep growing on the least loaded one so
    // a single-edge host (or a very fast edge) can still be saturated.
    for (size_t i = 0; i < edges_.size(); ++i) {
      const Slot& s = edges_[i];
      if (s.dead) continue;
      if (best < 0 || s.active < edges_[best].active ||
          (s.active == edges_[best].active && s.mibps > edges_[best].mibps))
        best = static_cast<int>(i);
    }
  }
  if (best < 0) return -1;
  ++edges_[best].active;
  return best;
}

void EndpointPool::release(int idx) {
  std::lock_guard<std::mutex> lock(mu_);
  if (idx < 0 || static_cast<size_t>(idx) >= edges_.size()) return;
  Slot& s = edges_[idx];
  if (s.active > 0) --s.active;
}

void EndpointPool::add_bytes(int idx, uint64_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  if (idx < 0 || static_cast<size_t>(idx) >= edges_.size()) return;
  Slot& s = edges_[idx];
  s.bytes += bytes;
  s.failures = 0;  // the edge works
}

void EndpointPool::note_failure(int idx) {
  std::lock_guard<std::mutex> lock(mu_);
  if (idx < 0 || static_cast<size_t>(idx) >= edges_.size()) return;
  Slot& s = edges_[idx];
  if (++s.failures >= kFailuresToRetire) s.dead = true;
}

bool EndpointPool::is_dead(size_t idx) const {
  std::lock_guard<std::mutex> lock(mu_);
  return idx >= edges_.size() || edges_[idx].dead;
}

void EndpointPool::mark_dead(int idx) {
  std::lock_guard<std::mutex> lock(mu_);
  if (idx >= 0 && static_cast<size_t>(idx) < edges_.size()) edges_[idx].dead = true;
}

EndpointPool::EdgeView EndpointPool::view(size_t idx) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (idx >= edges_.size()) return EdgeView{};
  const Slot& s = edges_[idx];
  return EdgeView{s.ep, s.active, s.bytes, s.mibps, s.dead};
}

void EndpointPool::sample_rates() {
  std::lock_guard<std::mutex> lock(mu_);
  const auto now = std::chrono::steady_clock::now();
  const double dt = std::chrono::duration<double>(now - last_sample_).count();
  if (dt <= 0.0) return;
  for (auto& s : edges_) {
    const uint64_t delta = s.bytes - s.mark;
    s.mark = s.bytes;
    // Light smoothing: rates are used to rank edges, not for reporting.
    const double inst = (static_cast<double>(delta) / (1024.0 * 1024.0)) / dt;
    s.mibps = s.mibps > 0.0 ? 0.5 * s.mibps + 0.5 * inst : inst;
  }
  last_sample_ = now;
}

}  // namespace mtdl
