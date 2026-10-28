#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "mtdl/net/endpoint.hpp"

namespace mtdl {

// The downloader's view of the edge set.
//
// Every worker *leases* one edge for the lifetime of its connection:
// acquire() hands out the least busy edge (preferring edges still below a soft
// limit, and faster edges among equals), release() gives the slot back. Byte
// counters per edge let the orchestrator see which edges actually deliver and
// stop feeding edges that keep failing before their first byte.
class EndpointPool {
 public:
  explicit EndpointPool(std::vector<Endpoint> edges, size_t soft_limit_per_edge);

  size_t size() const { return edges_.size(); }
  const Endpoint& endpoint(size_t idx) const { return edges_[idx].ep; }

  // Number of edges still considered usable.
  size_t healthy() const;

  // Reserves a connection slot. Returns the edge index, or -1 when all edges
  // are dead (nothing left to try).
  int acquire();

  // Returns a slot previously obtained from acquire().
  void release(int idx);

  void add_bytes(int idx, uint64_t bytes);
  // Counts a failure that happened *before* any byte arrived on this edge.
  void note_failure(int idx);
  bool is_dead(size_t idx) const;
  void mark_dead(int idx);

  struct EdgeView {
    Endpoint ep;
    size_t active = 0;
    uint64_t bytes = 0;
    double mibps = 0.0;  // measured over the latest sampling interval
    bool dead = false;
  };
  EdgeView view(size_t idx) const;

  // Recomputes per-edge throughput over the interval since the previous call.
  void sample_rates();

 private:
  struct Slot {
    Endpoint ep;
    size_t active = 0;
    uint64_t bytes = 0;
    uint64_t mark = 0;      // bytes at the previous sampling point
    double mibps = 0.0;
    uint32_t failures = 0;  // consecutive failures with no data received
    bool dead = false;
  };

  mutable std::mutex mu_;
  std::vector<Slot> edges_;
  size_t soft_limit_;
  std::chrono::steady_clock::time_point last_sample_;
};

}  // namespace mtdl
