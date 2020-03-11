/** \file

    Estimate the Boost Fiber context switch duration

    The use case is for circuit emulation when there are a lot of
    fibers launched at the beginning and they have to run concurrently.

    We are *not* interested in the launch/join behaviour exposed for
    example in https://github.com/atemerev/skynet since it is assumed
    to be amortize on the global long running time.
*/

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <boost/fiber/all.hpp>
#include <boost/thread/barrier.hpp>
#include <range/v3/all.hpp>

/// To select some various Boost.Fibers schedulers
enum class sched {
  round_robin,
  shared_work,
  work_stealing
  // \todo Add numa
};

//auto constexpr starting_mode = boost::fibers::launch::dispatch;
auto constexpr starting_mode = boost::fibers::launch::post;

// Use precise time measurement
using clk = std::chrono::high_resolution_clock;

/// Install the requested scheduler in the thread executing this function
void install_fiber_scheduler(sched scheduler, int thread_number, bool suspend) {
  if (scheduler == sched::shared_work)
    boost::fibers::use_scheduling_algorithm
      <boost::fibers::algo::shared_work>();
  if (scheduler == sched::work_stealing)
    boost::fibers::use_scheduling_algorithm
      <boost::fibers::algo::work_stealing>(thread_number, suspend);
}

/// A parametric benchmark
void bench_mark(int thread_number,
                int fiber_number,
                int iterations,
                sched scheduler,
                bool suspend) {
  std::cout << "threads: " << thread_number
            << " fibers: "<< fiber_number
            << " iterations: " << iterations
            << " scheduler: " << static_cast<int>(scheduler)
            << " suspend: " << static_cast<int>(suspend) << std::flush;

  /// The basic benchmark is fiber doing a lot of yield()
  auto bench = [&] {
                 for (auto counter = iterations; counter != 0; --counter)
                   boost::this_fiber::yield();
               };

  /// To synchronize all the threads before they can run some fibers
  boost::barrier b { static_cast<unsigned int>(thread_number) };

  // Just to block the working threads while there is some work to do
  boost::fibers::unbuffered_channel<int> blocker;

  auto starting_point = clk::now();

  // Start the working threads
  auto threads = ranges::iota_view { 0, thread_number }
               | ranges::views::transform([&] (auto i) {
    return std::async(std::launch::async,
                      [&] {
      // A thread cannot have another scheduler installed, so do it in
      // a brand new thread
      install_fiber_scheduler(scheduler,
                              thread_number,
                              suspend);
      // Wait for all thread workers to be ready
      b.count_down_and_wait();
      if (i == 0) {
        // The first thread start fiber_number fibers running bench
        auto fibers = ranges::iota_view { 0, fiber_number }
                    | ranges::views::transform([&] (auto i) {
                        return boost::fibers::fiber { starting_mode, bench }; })
                    | ranges::to<std::vector>;
        // Wait for everybody to finish
        for (auto &f : fibers)
          f.join();
        // Unleash the other threads
        blocker.close();
      }
      else
        try {
          // The threads block on the fiber scheduler until this throws
          // because the channel is closed. The fiber scheduler might
          // schedule some fibers behind the scene
          (void) blocker.value_pop();
        } catch(...) {}
     }); })
                   | ranges::to<std::vector>;

  for (auto &t : threads)
    t.get();

  // Get the duration in seconds as a double
  std::chrono::duration<double> duration = clk::now() - starting_point;
  // In s
  std::cout << " time: " << duration.count()
            <<" inter context switch: "
    // In ns
            << duration.count()/iterations/fiber_number*1e9 << std::endl;
}

int main() {
  for (int thread_number = 1;
       thread_number <= 2*std::thread::hardware_concurrency();
       ++thread_number)
    for (auto fiber_number : { 1, 3, 10, 30, 100, 300, 1000, 3000 })
      for (auto iterations : { 1e4, 1e5, 1e6 })
        for (auto scheduler :
             { sched::round_robin, sched::shared_work, sched::work_stealing }) {
          bench_mark(thread_number,
                     fiber_number,
                     iterations,
                     scheduler,
                     false);
          if (scheduler == sched::work_stealing)
            bench_mark(thread_number,
                       fiber_number,
                       iterations,
                       scheduler,
                       true);
        }
}
