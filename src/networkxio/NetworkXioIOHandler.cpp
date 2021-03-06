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

#include "NetworkXioIOHandler.h"
#include <sys/epoll.h>
#include <networkxio/NetworkXioCommon.h>
#include "NetworkXioProtocol.h"
#include "NetworkXioWorkQueue.h"

using namespace std;
namespace gobjfs {
namespace xio {

static constexpr int XIO_COMPLETION_DEFAULT_MAX_EVENTS = 100;

static inline void pack_msg(NetworkXioRequest *req) {
  NetworkXioMsg o_msg(req->op);
  o_msg.retval(req->retval);
  o_msg.errval(req->errval);
  o_msg.opaque(req->opaque);
  req->s_msg = o_msg.pack_msg();
}

NetworkXioIOHandler::NetworkXioIOHandler(IOExecServiceHandle serviceHandle,
                                         NetworkXioWorkQueuePtr wq)
    : serviceHandle_(serviceHandle), wq_(wq) {
  try {

    eventHandle_ = IOExecEventFdOpen(serviceHandle_);
    if (eventHandle_ == nullptr) {
      throw std::runtime_error("failed to open event handle");
    }

    auto efd = IOExecEventFdGetReadFd(eventHandle_);
    if (efd == -1) {
      throw std::runtime_error("failed to get read fd");
    }

    epollfd = epoll_create1(0);
    if (epollfd < 0) {
      throw std::runtime_error("epoll create failed with errno " + std::to_string(errno));
    }

    struct epoll_event event;
    event.data.fd = efd;
    event.events = EPOLLIN;
    int err = epoll_ctl(epollfd, EPOLL_CTL_ADD, efd, &event);
    if (err != 0) {
      throw std::runtime_error("epoll_ctl failed with error " + std::to_string(errno));
    }

    err = ioCompletionThreadShutdown.init(epollfd);
    if (err != 0) {
      throw std::runtime_error("failed to init shutdown notifier " + std::to_string(err));
    }

    ioCompletionThread = std::thread(std::bind(
        &NetworkXioIOHandler::gxio_completion_handler, this, epollfd, efd));

  } catch (std::exception& e) {
    GLOG_ERROR("failed to init handler " << e.what());
  }
}

NetworkXioIOHandler::~NetworkXioIOHandler() {
  try {
    int err = ioCompletionThreadShutdown.send();
    if (err != 0) {
      GLOG_ERROR("failed to notify completion thread");
    } else {
      ioCompletionThread.join();
    }

    ioCompletionThreadShutdown.destroy();

  } catch (const std::exception &e) {
    GLOG_ERROR("failed to join completion thread");
  }

  if (eventHandle_) {
    IOExecEventFdClose(eventHandle_);
    eventHandle_ = nullptr;
  }

  if (epollfd != -1) {
    close(epollfd);
    epollfd = -1;
  }
}

void NetworkXioIOHandler::handle_open(NetworkXioRequest *req) {
  int err = 0;
  GLOG_DEBUG("trying to open volume ");

  req->op = NetworkXioMsgOpcode::OpenRsp;
  req->retval = 0;
  req->errval = 0;

  pack_msg(req);
}

int NetworkXioIOHandler::gxio_completion_handler(int epollfd, int efd) {

  const unsigned int max = XIO_COMPLETION_DEFAULT_MAX_EVENTS;
  epoll_event events[max];

  bool mustExit = false;

  while (!mustExit) {

    int n = epoll_wait(epollfd, events, max, -1);

    for (int i = 0; i < n; i++) {

      if (events[i].data.ptr == &ioCompletionThreadShutdown) {
        uint64_t counter;
        ioCompletionThreadShutdown.recv(counter);
        mustExit = true;
        GLOG_DEBUG("Received shutdown event for ptr=" << (void *)this);
        continue;
      }

      if (efd != events[i].data.fd) {
        GLOG_ERROR("Received event for unknown fd="
                   << static_cast<uint32_t>(events[i].data.fd));
        continue;
      }

      gIOStatus iostatus;
      int ret = read(efd, &iostatus, sizeof(iostatus));

      if (ret != sizeof(iostatus)) {
        GLOG_ERROR("Partial read detected.  Actual read=" << ret << " Expected=" << sizeof(iostatus));
        continue;
      }

      GLOG_DEBUG("Recieved event"
                 << " completionId: " << (void *)iostatus.completionId
                 << " status: " << iostatus.errorCode);

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

        NetworkXioWorkQueue *pWorkQueue =
            reinterpret_cast<NetworkXioWorkQueue *>(pXioReq->req_wq);
        pWorkQueue->worker_bottom_half(pWorkQueue, pXioReq);
      } break;

      default: {
        GLOG_ERROR("Got an event for non-read operation "
                   << (int)pXioReq->op);
      }
      }
    }
  }

  return 0;
}

void NetworkXioIOHandler::handle_close(NetworkXioRequest *req) {
  req->op = NetworkXioMsgOpcode::CloseRsp;
  if (!serviceHandle_) {
    GLOG_ERROR("Device handle null for device ");
    req->retval = -1;
    req->errval = EIO;
  } else {
    req->retval = 0;
    req->errval = 0;
  }
  pack_msg(req);
}

int NetworkXioIOHandler::handle_read(NetworkXioRequest *req,
                                     const std::string &filename, size_t size,
                                     off_t offset) {
  int ret = 0;
  req->op = NetworkXioMsgOpcode::ReadRsp;
  req->req_wq = (void *)this->wq_.get();
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

  ret = xio_mempool_alloc(req->pClientData->ncd_mpool, size, &req->reg_mem);
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
     << " wq_ptr=" << req->req_wq 
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

    ret = IOExecFileRead(serviceHandle_, filename.c_str(), filename.size(),
                         batch, eventHandle_);

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
  NetworkXioWorkQueue *pWorkQueue = NULL;
  xio_msg *xio_req = req->xio_req;
  xio_iovec_ex *isglist = vmsg_sglist(&xio_req->in);
  int inents = vmsg_sglist_nents(&xio_req->in);

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
    case NetworkXioMsgOpcode::CloseReq: {
      GLOG_DEBUG(" Command CloseReq");
      handle_close(req);
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

void NetworkXioIOHandler::handle_request(NetworkXioRequest *req) {
  req->work.func = std::bind(&NetworkXioIOHandler::process_request, this, req);
  wq_->work_schedule(req);
}
}
} // namespace gobjfs
