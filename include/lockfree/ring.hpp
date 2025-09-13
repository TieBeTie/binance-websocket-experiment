#pragma once

#include <boost/lockfree/spsc_queue.hpp>
#include <cstddef>
#include <utility>

// lockfree::Ring — zero-allocation SPSC object recycler built on two SPSC
// queues:
// - free_: holds reusable slots (T objects owned by the ring)
// - ready_: holds produced items for the consumer
// Usage pattern (single producer, single consumer):
//   Producer: acquire(out) → fill out → publish(std::move(out))
//   Consumer: consume(out) → process out → release(std::move(out))
// On construction the ring pre-populates free_ with Capacity
// default-constructed T to avoid allocations at runtime. T must be movable.
namespace lockfree {

template <typename T, std::size_t Capacity> class Ring {
public:
  using value_type = T;
  static constexpr std::size_t kCapacity = Capacity;
  using Queue =
      boost::lockfree::spsc_queue<T, boost::lockfree::capacity<kCapacity>>;

  Ring() {
    // Pre-populate free_ with Capacity default-constructed objects
    for (std::size_t i = 0; i < kCapacity; ++i) {
      T slot{};
      (void)free_.push(std::move(slot));
    }
  }

  // Non-copyable, movable
  Ring(const Ring &) = delete;
  Ring &operator=(const Ring &) = delete;
  Ring(Ring &&) = default;
  Ring &operator=(Ring &&) = default;

  // Producer API
  // Try to acquire an empty slot from free_. Returns false if none available.
  bool acquire(T &out) { return free_.pop(out); }

  // Publish a filled item to the ready_ queue. Returns false if queue is full.
  bool publish(T &&item) { return ready_.push(std::move(item)); }

  // Consumer API
  // Try to consume the next ready item. Returns false if none available.
  bool consume(T &out) { return ready_.pop(out); }

  // Release a processed item back to free_. Returns false if free_ is full
  // (should not happen if used as designed with matching
  // acquire/publish/consume/release).
  bool release(T &&item) { return free_.push(std::move(item)); }

  // Introspection (approximate counts)
  std::size_t ready_size() const { return ready_.read_available(); }
  std::size_t free_size() const { return free_.read_available(); }

private:
  // Separate queues prevent false sharing of head/tail indices across roles
  Queue free_;
  Queue ready_;
};

} // namespace lockfree
