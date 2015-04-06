/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "mcrouter/lib/fbi/cpp/LogFailure.h"

namespace facebook { namespace memcache {

namespace {
#ifndef LIBMC_FBTRACE_DISABLE

template <class Request>
typename std::enable_if<RequestHasFbTraceInfo<Request>::value,
                        const mc_fbtrace_info_s*>::type
inline getFbTraceInfo(const Request& request) {
  return request.fbtraceInfo();
}

template <class Request>
typename std::enable_if<!RequestHasFbTraceInfo<Request>::value,
                        const mc_fbtrace_info_s*>::type
inline getFbTraceInfo(const Request& request) {
  return nullptr;
}

#endif
}

template <class Reply>
bool McClientRequestContextBase::reply(Reply&& r) {
  assert(state_ == ReqState::PENDING_REPLY_QUEUE);
  if (replyType_ != typeid(Reply)) {
    failure::log("AsyncMcClient", failure::Category::kBrokenLogic,
                 "Attempt to forward a reply of a wrong type. Expected '{}', "
                 "but received '{}'!", replyType_.name(), typeid(Reply).name());

    return false;
  }

  auto* storage = reinterpret_cast<folly::Optional<Reply>*>(replyStorage_);
  assert(!storage->hasValue());
  storage->emplace(std::move(r));
  sendTraceOnReply();
  state_ = ReqState::COMPLETE;
  baton_.post();
  return true;
}

template <class Operation, class Request>
McClientRequestContextBase::McClientRequestContextBase(
  Operation, const Request& request, uint64_t reqid, mc_protocol_t protocol,
  std::shared_ptr<AsyncMcClientImpl> client,
  folly::Optional<typename ReplyType<Operation, Request>::type>& replyStorage,
  McClientRequestContextQueue& queue, InitializerFuncPtr initializer)
  : reqContext(request, Operation(), reqid, protocol),
    id(reqid),
    queue_(queue),
    client_(std::move(client)),
    replyType_(typeid(typename ReplyType<Operation, Request>::type)),
    replyStorage_(reinterpret_cast<void*>(&replyStorage)),
    initializer_(std::move(initializer)) {
}

template <class Operation, class Request>
void McClientRequestContext<Operation, Request>::replyError(
    mc_res_t result) {
  assert(state_ == ReqState::NONE);
  assert(!replyStorage_.hasValue());
  replyStorage_.emplace(result);
  state_ = ReqState::COMPLETE;
  baton_.post();
}

template <class Operation, class Request>
const char*
McClientRequestContext<Operation, Request>::fakeReply() const {
  return "CLIENT_ERROR unsupported operation\r\n";
}

template <class Operation, class Request>
typename McClientRequestContext<Operation, Request>::Reply
McClientRequestContext<Operation, Request>::waitForReply(
    std::chrono::milliseconds timeout) {

  if (timeout.count()) {
    baton_.timed_wait(timeout);
  } else {
    baton_.wait();
  }

  switch (state_) {
    case ReqState::WRITE_QUEUE:
      // Request is being written into socket, we need to wait for it to be
      // completely written, then reply with timeout.
      state_ = ReqState::WRITE_QUEUE_CANCELED;
      baton_.reset();
      baton_.wait();
      return Reply(mc_res_timeout);
    case ReqState::PENDING_QUEUE:
      // Request wasn't sent to the network yet, reply with timeout.
      queue_.removePending(*this);
      return Reply(mc_res_timeout);
    case ReqState::PENDING_REPLY_QUEUE:
      // Request was sent to the network, but wasn't replied yet,
      // reply with timeout.
      queue_.removePendingReply(*this);
      return Reply(mc_res_timeout);
    case ReqState::COMPLETE:
      assert(replyStorage_.hasValue());
      return std::move(replyStorage_.value());
    case ReqState::WRITE_QUEUE_CANCELED:
    case ReqState::NONE:
      failure::log("AsyncMcClient", failure::Category::kBrokenLogic,
                   "Unexpected state of request: {}!",
                   static_cast<uint64_t>(state_));
  }
  return Reply(mc_res_local_error);
}

template <class Operation, class Request>
McClientRequestContext<Operation, Request>::McClientRequestContext(
  const Request& request, uint64_t reqid, mc_protocol_t protocol,
  std::shared_ptr<AsyncMcClientImpl> client, McClientRequestContextQueue& queue,
  McClientRequestContextBase::InitializerFuncPtr func)
  : McClientRequestContextBase(Operation(), request, reqid, protocol,
                               std::move(client), replyStorage_, queue,
                               std::move(func))
#ifndef LIBMC_FBTRACE_DISABLE
    , fbtraceInfo_(getFbTraceInfo(request))
#endif
{
}

template <class Operation, class Request>
void McClientRequestContext<Operation, Request>::sendTraceOnReply() {
#ifndef LIBMC_FBTRACE_DISABLE
  fbTraceOnReceive(Operation(), fbtraceInfo_, replyStorage_.value());
#endif
}

template <class Reply>
void McClientRequestContextQueue::reply(uint64_t id, Reply&& r) {
  // Get the context and erase it from the queue and map.
  McClientRequestContextBase* ctx{nullptr};
  if (outOfOrder_) {
    auto iter = idMap_.find(id);
    if (iter != idMap_.end()) {
      ctx = iter->second;
      assert(ctx->state_ == State::PENDING_REPLY_QUEUE);
      pendingReplyQueue_.erase(pendingReplyQueue_.iterator_to(*iter->second));
      idMap_.erase(iter);
    }
  } else {
    // First we're going to receive replies for timed out requests.
    if (!timedOutInitializers_.empty()) {
      timedOutInitializers_.pop();
    } else if (!pendingReplyQueue_.empty()) {
      ctx = &pendingReplyQueue_.front();
      pendingReplyQueue_.pop_front();
    }
    // With old mc_parser it's possible to receive unexpected replies, we need
    // to ignore them.
  }

  if (ctx) {
    if (!ctx->reply(std::move(r))) {
      ctx->state_ = State::NONE;
      ctx->replyError(mc_res_local_error);
    }
  }
}

}}  // facebook::memcache