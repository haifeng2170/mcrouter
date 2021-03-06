/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "mcrouter/lib/network/CongestionController.h"

namespace facebook {
namespace memcache {

class CpuController : public std::enable_shared_from_this<CpuController> {
 public:
  CpuController(
      uint64_t target,
      folly::EventBase& evb,
      std::chrono::milliseconds delay = std::chrono::milliseconds(100),
      size_t queueCapacity = 1000);

  double getDropProbability() const;

  void start();
  void stop();

 private:
  // The function responsible for logging the CPU utilization.
  void cpuLoggingFn();

  folly::EventBase& evb_;
  bool firstLoop_{true};
  std::atomic<bool> stopController_{false};
  std::vector<uint64_t> prev_{4};
  std::shared_ptr<CongestionController> logic_;
};

} // memcache
} // facebook
