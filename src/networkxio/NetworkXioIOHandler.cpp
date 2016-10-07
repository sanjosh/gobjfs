/*
Copyright (C) 2016 iNuron NV

This file is part of Open vStorage Open Source Edition (OSE), as available from


    http://www.openvstorage.org and
    http://www.openvstorage.com.

This file is free software; you can redistribute it and/or modify it
under the terms of the GNU Affero General Public License v3 (GNU AGPLv3)
as published by the Free Software Foundation, in version 3 as it comes
in the <LICENSE.txt> file of the Open vStorage OSE distribution.

Open vStorage is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY of any kind.
*/

#include <unistd.h>

#include <networkxio/gobjfs_client_common.h>
#include <gobjfs_client.h>
#include <IOExecutor.h>

#include "gobjfs_getenv.h"
#include "NetworkXioIOHandler.h"
#include "NetworkXioCommon.h"
#include "NetworkXioProtocol.h"
#include "NetworkXioServer.h"
#include "PortalThreadData.h"

#include <util/lang_utils.h>

using namespace std;
namespace gobjfs {
namespace xio {


static inline void pack_msg(NetworkXioRequest *req) {
  NetworkXioMsg o_msg(req->op);
  o_msg.retval(req->retval);
  o_msg.errval(req->errval);
  o_msg.opaque(req->opaque);
  req->s_msg = o_msg.pack_msg();
}

/** 
 * called from accelio event loop
 */
template <class T> 
static void static_disk_event(int fd, int events, void *data) {
  T *obj = reinterpret_cast<T *>(data);
  if (obj == NULL) {
    return;
  }
  obj->handleXioEvent(fd, events, data);
}

/** 
 * called from accelio event loop
 */
template <class T> 
static void static_timer_event(int fd, int events, void *data) {
  T *obj = reinterpret_cast<T *>(data);
  if (obj == NULL) {
    return;
  }
  obj->runTimerHandler();
}


NetworkXioIOHandler::NetworkXioIOHandler(PortalThreadData* pt)
  : pt_(pt) {

  // just create one IOExecutor and bind it to this handler
  const int numCores = 1;

  serviceHandle_ = IOExecFileServiceInit(numCores, 
        pt_->server_->queueDepthForIO_,
        pt_->server_->fileTranslatorFunc_, 
        pt_->server_->newInstance_);

  if (serviceHandle_ == nullptr) {
    throw std::bad_alloc();
  }

  // TODO smart ptr
  eventHandle_ = IOExecEventFdOpen(serviceHandle_);

  eventFD_ = IOExecEventFdGetReadFd(eventHandle_);

  uint64_t timerSec = getenv_with_default("GOBJFS_SERVER_TIMER_SEC", 60);
  const int64_t timerNanosec = 0;
  statsTimerFD_ = gobjfs::make_unique<TimerNotifier>(timerSec, timerNanosec);

  startEventHandler();
}

NetworkXioIOHandler::~NetworkXioIOHandler() {

  stopEventHandler();

  IOExecEventFdClose(eventHandle_);

  if (serviceHandle_) {
    IOExecFileServiceDestroy(serviceHandle_);
    serviceHandle_ = nullptr;
  }

}

void NetworkXioIOHandler::startEventHandler() {

  try {

    ioexecPtr_ = static_cast<IOExecutor*>(IOExecGetExecutorPtr(serviceHandle_, 0));

    if (xio_context_add_ev_handler(pt_->ctx_, eventFD_,
                                XIO_POLLIN,
                                static_disk_event<gobjfs::IOExecutor>,
                                (void*)ioexecPtr_)) {

      throw FailedRegisterEventHandler("failed to register event handler");

    }

    GLOG_INFO("registered disk event fd=" << eventFD_
        << ",ioexecutor=" << ioexecPtr_
        << ",thread=" << gettid());

    // Add periodic timer to print per-thread stats
    if (xio_context_add_ev_handler(pt_->ctx_, statsTimerFD_->getFD(),
                                XIO_POLLIN,
                                static_timer_event<NetworkXioIOHandler>,
                                this)) {

      throw FailedRegisterEventHandler("failed to register event handler");

    }

    GLOG_INFO("registered timer event fd=" << statsTimerFD_->getFD()
        << ",thread=" << gettid());

  } catch (std::exception& e) {
    GLOG_ERROR("failed to init handler " << e.what());
  }
}

/** 
 * called from FilerJob::reset after submitted IO is complete
 */
static int static_runEventHandler(gIOStatus& iostatus, void* ctx) {
  NetworkXioIOHandler* handler = (NetworkXioIOHandler*)ctx;
  handler->runEventHandler(iostatus);
  return 0;
}

/**
 * this runs in the context of the portal thread
 */
int NetworkXioIOHandler::runEventHandler(gIOStatus& iostatus) {

  gIOBatch *batch = reinterpret_cast<gIOBatch *>(iostatus.completionId);
  assert(batch != nullptr);

  NetworkXioRequest *pXioReq =
      static_cast<NetworkXioRequest *>(batch->opaque);
  assert(pXioReq != nullptr);

  gIOExecFragment &frag = batch->array[0];
  // reset addr otherwise BatchFree will free it
  // need to introduce ownership indicator
  frag.addr = nullptr;
  gIOBatchFree(batch);

  switch (pXioReq->op) {

  case NetworkXioMsgOpcode::ReadRsp: {

    if (iostatus.errorCode == 0) {
      // read must return the size which was read
      pXioReq->retval = pXioReq->size;
      pXioReq->errval = 0;
      GLOG_DEBUG(" Read completed with completion ID"
                 << iostatus.completionId);
    } else {
      pXioReq->retval = -1;
      pXioReq->errval = iostatus.errorCode;
      GLOG_ERROR("Read completion error " << iostatus.errorCode
                                          << " For completion ID "
                                          << iostatus.completionId);
    }

    pack_msg(pXioReq);

    pt_->server_->send_reply(pXioReq);

  } break;

  default: 
    GLOG_ERROR("Got an event for non-read operation "
               << (int)pXioReq->op);
  }
  return 0;
}

void NetworkXioIOHandler::stopEventHandler() {
  GLOG_INFO("deregistered io fd=" << eventFD_ << " for thread=" << gettid());
  xio_context_del_ev_handler(pt_->ctx_, eventFD_);

  if (statsTimerFD_) {
    GLOG_INFO("deregistered timer fd=" << statsTimerFD_->getFD() << " for thread=" << gettid());
    xio_context_del_ev_handler(pt_->ctx_, statsTimerFD_->getFD());
  }
}

void NetworkXioIOHandler::runTimerHandler()
{
  // print logs every 60 sec
  // first drain the timer
  uint64_t count = 0;
  statsTimerFD_->recv(count);

  GLOG_INFO("thread=" << gettid() 
      << ",portalId=" << pt_->coreId_ 
      << ",numConnections=" << pt_->numConnections()
      << ",ioexec=" << ioexecPtr_->stats_.getState());

  ioexecPtr_->stats_.clear();
}

void NetworkXioIOHandler::handle_open(NetworkXioRequest *req) {
  GLOG_DEBUG("trying to open volume ");

  req->op = NetworkXioMsgOpcode::OpenRsp;
  req->retval = 0;
  req->errval = 0;

  pack_msg(req);
}

int NetworkXioIOHandler::handle_read(NetworkXioRequest *req,
                                     const std::string &filename, size_t size,
                                     off_t offset) {


  int ret = 0;
  req->op = NetworkXioMsgOpcode::ReadRsp;
#ifdef BYPASS_READ
  { 
    req->retval = size;
    req->errval = 0;
    pack_msg(req);
    return 0;
  } 
#endif
  if (!serviceHandle_) {
    GLOG_ERROR("no service handle");
    req->retval = -1;
    req->errval = EIO;
    pack_msg(req);
    return -1;
  }

  ret = xio_mempool_alloc(pt_->mpool_, size, &req->reg_mem);
  if (ret < 0) {
    // could not allocate from mempool, try mem alloc
    ret = xio_mem_alloc(size, &req->reg_mem);
    if (ret < 0) {
      GLOG_ERROR("cannot allocate requested buffer, size: " << size);
      req->retval = -1;
      req->errval = ENOMEM;
      pack_msg(req);
      return ret;
    }
    req->from_pool = false;
  } else {
    req->from_pool = true;
  }

  GLOG_DEBUG("Received read request for object "
     << " file=" << filename 
     << " at offset=" << offset 
     << " for size=" << size);

  req->data = req->reg_mem.addr;
  req->data_len = size;
  req->size = size;
  req->offset = offset;
  try {

    memset(static_cast<char *>(req->data), 0, req->size);

    gIOBatch *batch = gIOBatchAlloc(1);
    batch->opaque = req;
    gIOExecFragment &frag = batch->array[0];

    frag.offset = offset;
    frag.addr = reinterpret_cast<caddr_t>(req->reg_mem.addr);
    frag.size = size;
    frag.completionId = reinterpret_cast<uint64_t>(batch);

    // passing cd->coreId to the job allows the IOExecutor to execute
    // job on the same core as the accelio portal thread on which it
    // was received.  Not getting any perf improvement with this so
    // disabling it.
    ret = IOExecFileRead(serviceHandle_, filename.c_str(), filename.size(),
                         batch, 
                         eventFD_,
                         static_runEventHandler,
                         (void*)this);

    if (ret != 0) {
      GLOG_ERROR("IOExecFileRead failed with error " << ret);
      req->retval = -1;
      req->errval = EIO;
      frag.addr = nullptr;
      gIOBatchFree(batch);
    }

    // CAUTION : only touch "req" after this if ret is non-zero
    // because "req" can get freed by the other thread if the IO completed
    // leading to a "use-after-free" memory error
  } catch (...) {
    GLOG_ERROR("failed to read volume ");
    req->retval = -1;
    req->errval = EIO;
  }

  if (ret != 0) {
    pack_msg(req);
  }
  return ret;
}

void NetworkXioIOHandler::handle_error(NetworkXioRequest *req, int errval) {
  req->op = NetworkXioMsgOpcode::ErrorRsp;
  req->retval = -1;
  req->errval = errval;
  pack_msg(req);
}

/*
 * @returns finishNow indicates whether response can be immediately sent to client 
 */
bool NetworkXioIOHandler::process_request(NetworkXioRequest *req) {
  bool finishNow = true;
  xio_msg *xio_req = req->xio_req;
  //xio_iovec_ex *isglist = vmsg_sglist(&xio_req->in);
  //int inents = vmsg_sglist_nents(&xio_req->in);

  NetworkXioMsg i_msg(NetworkXioMsgOpcode::Noop);
  try {
    i_msg.unpack_msg(static_cast<char *>(xio_req->in.header.iov_base),
                     xio_req->in.header.iov_len);
  } catch (...) {
    GLOG_ERROR("cannot unpack message");
    handle_error(req, EBADMSG);
    return finishNow;
  }

  req->opaque = i_msg.opaque();
  switch (i_msg.opcode()) {
    case NetworkXioMsgOpcode::OpenReq: {
      GLOG_DEBUG(" Command OpenReq");
      handle_open(req);
      break;
    }
    case NetworkXioMsgOpcode::ReadReq: {
      GLOG_DEBUG(" Command ReadReq");
      auto ret = handle_read(req, i_msg.filename_, i_msg.size(), i_msg.offset());
      if (ret == 0) {
        finishNow = false;
      }
#ifdef BYPASS_READ
      finishNow = true;
#endif
      break;
    }
    default:
      GLOG_ERROR("Unknown command " << (int)i_msg.opcode());
      handle_error(req, EIO);
  };
  return finishNow;
}

bool NetworkXioIOHandler::alreadyInvoked() {
  return (ioexecPtr_->requestQueueSize() > 0);
}

void NetworkXioIOHandler::drainQueue() {
  ioexecPtr_->ProcessRequestQueue();
}

// this func runs in context of portal thread
void NetworkXioIOHandler::handle_request(NetworkXioRequest *req) {
  process_request(req); 
  if (alreadyInvoked()) { 
    // stop the loop so that we do custom handling
    // in top-level loop
    xio_context_stop_loop(pt_->ctx_);
  }
}
}
} // namespace gobjfs
