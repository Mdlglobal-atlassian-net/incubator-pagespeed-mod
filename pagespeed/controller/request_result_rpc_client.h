/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */


#ifndef PAGESPEED_CONTROLLER_REQUEST_RESULT_RPC_CLIENT_H_
#define PAGESPEED_CONTROLLER_REQUEST_RESULT_RPC_CLIENT_H_

#include <memory>

#include "base/logging.h"
#include "pagespeed/controller/controller.grpc.pb.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/thread_annotations.h"
#include "pagespeed/kernel/util/grpc.h"

// RequestResultRpcClient manages the client portion of a gRPC connection. It is
// the client-side counterpart to RequestResultRpcHandler. See class comments
// below.

namespace net_instaweb {

// Holder of various bits of client-side gRPC stuff. Primarily exists
// to allow a blocking call to something like Done() into an async call to
// the server.
template <typename ReaderWriter>
class RpcHolder {
 public:
  RpcHolder(MessageHandler* handler) : handler_(handler) {}

  // Takes ownership of "this", ie: You must remove the RpcHolder from any smart
  // pointers because it's now going to delete itself.
  // You should only call one of CallbackForAsyncCleanup() or Finish().
  Function* CallbackForAsyncCleanup() {
    return MakeFunction(this, &RpcHolder::Finish, &RpcHolder::Error);
  }

  // Takes ownership of "this", ie: You must remove the RpcHolder from any smart
  // pointers because it's now going to delete itself.
  // You should only call one of CallbackForAsyncCleanup() or Finish().
  void Finish() {
    rw()->Finish(&status_, MakeFunction(this, &RpcHolder::FinishSucceeded,
                                        &RpcHolder::FinishFailed));
  }

  ::grpc::ClientContext* context() { return &context_; }

  MessageHandler* handler() { return handler_; }

  ReaderWriter* rw() { return rw_.get(); }

  void SetReaderWriter(std::unique_ptr<ReaderWriter> rw) {
    DCHECK(rw_ == nullptr);
    rw_ = std::move(rw);
  }

 private:
  void FinishSucceeded() {
    // OK and CANCELLED are expected error codes, don't bother to log them.
    if (status_.error_code() != ::grpc::StatusCode::OK &&
        status_.error_code() != ::grpc::StatusCode::CANCELLED) {
      MessageType severity =
#ifndef NDEBUG
          // ABORTED is generated by state machine errors on the server side,
          // so use kFatal for that in debug builds.
          (status_.error_code() == ::grpc::StatusCode::ABORTED) ? kFatal :
#endif
                                                                kWarning;
      handler_->Message(severity,
                        "Received error status from CentralController: %d (%s)",
                        status_.error_code(), status_.error_message().c_str());
    }
    delete this;
  }

  void FinishFailed() {
    PS_LOG_WARN(handler_, "RpcHolder Finish failed");
    delete this;
  }

  void Error() {
    PS_LOG_WARN(handler_, "RpcHolder cleanup to CentralController failed");
    Finish();  // We'd still like to see the error status. Will delete us.
  }

  MessageHandler* handler_;
  ::grpc::ClientContext context_;
  std::unique_ptr<ReaderWriter> rw_;
  ::grpc::Status status_;
};

// This is intended for use as the Context for a subclass of
// CentralControllerCallback (like ExpensiveOperationCallback or
// ScheduleRewriteCallback). Unfortunately it can't literally be the
// implementation of the Context due to double-inheritance, instead you probably
// want to put a subclass of this inside the Context and delegate to it. See
// ExpensiveOperationRpcContext for an example of this.
//
// Subclassers probably should call Start() right after they create an instace
// of this. Start() will trigger a series of gRPC calls to the server and
// eventually call either Run or Cancel on the callback supplied to the
// constructor. If Run is called, do your work and then call
// SendResultToServer() to let it know you are done. If Cancel() is called, do
// not do the work.
template <typename RequestT, typename ResponseT, typename CallbackT>
class RequestResultRpcClient {
 public:
  typedef ::grpc::ClientAsyncReaderWriterInterface<RequestT, ResponseT>
      ReaderWriter;

  RequestResultRpcClient(
      ::grpc::CompletionQueue* queue, ThreadSystem* thread_system,
      MessageHandler* handler, CallbackT* callback)
      : mutex_(thread_system->NewMutex()),
        queue_(queue),
        callback_(callback),
        rpc_(new RpcHolder<ReaderWriter>(handler)) {
    CHECK(callback_ != nullptr);
  }

  virtual ~RequestResultRpcClient() {
    // The child's destructor should probably call SendResultToServerIfActive
    // to ensure proper cleanup happens. This isn't strictly required as the
    // server will notice the hangup and handle it gracefully, but it may log
    // errors in the process.
  }

  // Actually start the RPC by having the client call RequestFoo on the stub.
  void Start(grpc::CentralControllerRpcService::StubInterface* stub) {
    ScopedMutex lock(mutex_.get());
    rpc_->SetReaderWriter(
        StartRpc(stub, rpc_->context(), queue_,
                 MakeFunction(this, &RequestResultRpcClient::BootStrapFinished,
                              &RequestResultRpcClient::StartUpFailed)));
  }

  // Call this once your client has completed their work, ie: they call
  // something like Done(), or at context destruction. You may call this method
  // as many times as you like, but only the first one will actually do
  // anything.
  void SendResultToServer(const RequestT& response) {
    ScopedMutex lock(mutex_.get());
    if (rpc_ == nullptr) {
      return;
    }
    // Detach the rpc context stuff and kick off the last Write() to the
    // server to let it know we're done. We do this "detached" because it's
    // very common that this method is invoked through the destructor of the
    // context, which shouldn't block. The AsyncCleanup callback just makes
    // sure the message was sent to the server and logs an error if not.
    RpcHolder<ReaderWriter>* rpc = rpc_.release();
    rpc->rw()->Write(response, rpc->CallbackForAsyncCleanup());
  }

 private:
  // Delegate for the client to call AsyncFoo for the appropriate RPC on the
  // stub.
  virtual std::unique_ptr<ReaderWriter> StartRpc(
      grpc::CentralControllerRpcService::StubInterface* stub,
      ::grpc::ClientContext* context, ::grpc::CompletionQueue* queue,
      void* tag) = 0;

  // Populate the initial RequestT that is sent to the server. This is the
  // message that is used to request work. The server's response will be
  // communicated back by a call to either Run() or Cancel() on the callback
  // that was supplied in the constructor.
  virtual void PopulateServerRequest(RequestT* request) = 0;

  // Handler for Start() success, above.
  void BootStrapFinished() {
    ScopedMutex lock(mutex_.get());
    RequestT req;
    PopulateServerRequest(&req);
    rpc_->rw()->Write(
        req,
        MakeFunction(this, &RequestResultRpcClient::WriteServerRequestComplete,
                     &RequestResultRpcClient::StartUpFailed));
  }

  void WriteServerRequestComplete() {
    ScopedMutex lock(mutex_.get());
    rpc_->rw()->Read(
        &resp_, MakeFunction(
                    this, &RequestResultRpcClient::NotifyClientOfServerDecision,
                    &RequestResultRpcClient::StartUpFailed));
  }

  void NotifyClientOfServerDecision() {
    ScopedMutex lock(mutex_.get());
    DCHECK(rpc_ != nullptr);
    // This could delegate to the subclass, but we're already relying on the
    // fact that this boolean has the same name on the server side.
    bool ok_to_proceed = resp_.ok_to_proceed();
    resp_.Clear();

    CallbackT* cb = callback_;
    callback_ = nullptr;

    if (ok_to_proceed) {
      lock.Release();
      cb->CallRun();
      return;
      // User will call back into us via SendResultToServer() at some point.
    } else {
      // Terminate session and disable calls to SendResultToServer.
      rpc_.reset();
      lock.Release();
      cb->CallCancel();
      return;
    }
  }

  // Handler for any errors up until the point NotifyClientOfServerDecision is
  // called.
  void StartUpFailed() LOCKS_EXCLUDED(mutex_) {
    ScopedMutex lock(mutex_.get());
    DCHECK(rpc_ != nullptr);
    if (rpc_ != nullptr) {
      PS_LOG_WARN(rpc_->handler(),
                  "Couldn't get response from CentralController");
      // Detach the rpc and call Finish to get (log) the error code in the
      // background. It is possible to avoid detaching if we delay the callback
      // until Finish completes. However, there's very little advantage to that,
      // and this means we can just use the same Finish() handler in the
      // RpcHolder.
      RpcHolder<ReaderWriter>* rpc = rpc_.release();
      rpc->Finish();

      Function* cb = callback_;
      callback_ = nullptr;

      lock.Release();
      cb->CallCancel();
      return;
    }
  }

  std::unique_ptr<AbstractMutex> mutex_;
  ::grpc::CompletionQueue* queue_;
  CallbackT* callback_ GUARDED_BY(mutex_);
  std::unique_ptr<RpcHolder<ReaderWriter>> rpc_ GUARDED_BY(mutex_);
  ResponseT resp_ GUARDED_BY(mutex_);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_CONTROLLER_REQUEST_RESULT_RPC_CLIENT_H_
