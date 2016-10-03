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

#include <libxio.h>
#include <future>
#include <mutex>
#include <sys/epoll.h> // epoll_create1

#include <networkxio/gobjfs_client_common.h>
#include <networkxio/gobjfs_config.h>
#include <gobjfs_client.h>
#include <util/os_utils.h>

#include "NetworkXioServer.h"
#include "NetworkXioProtocol.h"
#include "NetworkXioRequest.h"
#include "gobjfs_getenv.h"

using gobjfs::os::DirectIOSize;

namespace gobjfs {
namespace xio {

static constexpr int XIO_COMPLETION_DEFAULT_MAX_EVENTS = 100;

// TODO cleanup duplicate of function existing in NetworkXioIOHandler
static inline void pack_msg(NetworkXioRequest *req) {
  NetworkXioMsg o_msg(req->op);
  o_msg.retval(req->retval);
  o_msg.errval(req->errval);
  o_msg.opaque(req->opaque);
  req->s_msg = o_msg.pack_msg();
}

/**
 * Only called for Portal
 */
template <class T>
static int static_on_request(xio_session *session, xio_msg *req,
                             int last_in_rxq, void *cb_user_context) {
  T *obj = reinterpret_cast<T *>(cb_user_context);
  if (obj == NULL) {
    return -1;
  }
  return obj->ncd_server->on_request(session, req, last_in_rxq,
                                     cb_user_context);
}

/**
 * Only called for Portal
 */
template <class T>
static int static_on_msg_send_complete(xio_session *session, xio_msg *msg,
                                       void *cb_user_context) {
  XXEnter();
  T *obj = reinterpret_cast<T *>(cb_user_context);
  if (obj == NULL) {
    XXExit();
    return -1;
  }
  XXExit();
  return obj->ncd_server->on_msg_send_complete(session, msg, cb_user_context);
}

/**
 * Only called for Server
 */
template <class T>
static int static_on_session_event(xio_session *session,
                                   xio_session_event_data *event_data,
                                   void *cb_user_context) {
  NetworkXioClientData* cd = nullptr;

  // this is how you disambiguate between global and portal event
  if (event_data->conn_user_context == cb_user_context) {
    // this is a global server event
    GLOG_INFO("got global session event " << gettid());
  } else {
    // this is a portal event 
    cd = (NetworkXioClientData*)event_data->conn_user_context;
    GLOG_INFO("got portal session event " << gettid());
  }

  // TODO
  T *obj = reinterpret_cast<T *>(cb_user_context);
  if (obj == NULL) {
    return -1;
  }
  return obj->on_session_event(session, event_data, cd);
}

/**
 * Only called for Server
 */
template <class T>
static int static_on_new_session(xio_session *session, xio_new_session_req *req,
                                 void *cb_user_context) {
  XXEnter();
  T *obj = reinterpret_cast<T *>(cb_user_context);
  if (obj == NULL) {
    XXExit();
    return -1;
  }
  XXExit();
  return obj->on_new_session(session, req);
}



template <class T>
static void static_evfd_stop_loop(int fd, int events, void *data) {
  XXEnter();
  T *obj = reinterpret_cast<T *>(data);
  if (obj == NULL) {
    XXExit();
    return;
  }
  obj->evfd_stop_loop(fd, events, data);
  XXExit();
}

// TODO make it a member func of ClientData
void portal_func(void* data) {
  NetworkXioClientData* cd = (NetworkXioClientData*)data;
  
  // add cpu affinity later
  cd->ncd_ctx = xio_context_create(NULL, 0, -1);

  if (xio_context_add_ev_handler(cd->ncd_ctx, cd->evfd, XIO_POLLIN,
                                 static_evfd_stop_loop<NetworkXioClientData>,
                                 cd)) {
    throw FailedRegisterEventHandler("failed to register event handler");
  }

  xio_session_ops portal_ops;
  portal_ops.on_session_event = NULL;
  portal_ops.on_new_session = NULL;
  portal_ops.on_msg_send_complete = static_on_msg_send_complete<NetworkXioClientData>;
  portal_ops.on_msg = static_on_request<NetworkXioClientData>;

  cd->ncd_xio_server = xio_bind(cd->ncd_ctx, &portal_ops, cd->ncd_uri.c_str(),
      NULL, 0, cd);

  GLOG_INFO("started portal func at " << gettid());

  while (not cd->ncd_server->stopping) {
    xio_context_run_loop(cd->ncd_ctx, XIO_INFINITE);
    while (not cd->is_finished_empty()) {
      cd->ncd_server->send_reply(cd->get_finished());
    }
  }

  // TODO drop_ev_handler

  xio_unbind(cd->ncd_xio_server);

  xio_context_destroy(cd->ncd_ctx);
}

NetworkXioServer::NetworkXioServer(const std::string &transport,
                                    const std::string &ipaddr,
                                    int port,
                                   int32_t numCoresForIO,
                                   int32_t queueDepthForIO,
                                   FileTranslatorFunc fileTranslatorFunc,
                                   bool newInstance, size_t snd_rcv_queue_depth)
    : transport_(transport),
      ipaddr_(ipaddr),
      port_(port), 
      numCoresForIO_(numCoresForIO),
      queueDepthForIO_(queueDepthForIO),
      fileTranslatorFunc_(fileTranslatorFunc), newInstance_(newInstance),
      stopping(false), stopped(false), 
      queue_depth(snd_rcv_queue_depth) {}


void NetworkXioServer::xio_destroy_ctx_shutdown(xio_context *ctx) {
  xio_context_destroy(ctx);
  xio_shutdown();
}

NetworkXioServer::~NetworkXioServer() { shutdown(); }

// TODO same for Portal
void NetworkXioServer::evfd_stop_loop(int /*fd*/, int /*events*/,
                                      void * /*data*/) {
  evfd.readfd();
  xio_context_stop_loop(ctx.get());
}

void NetworkXioServer::Disk::startEventHandler(NetworkXioServer* server) {

  this->server_ = server;

  try {

    eventHandle_ = IOExecEventFdOpen(server->serviceHandle_);
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
        &NetworkXioServer::Disk::runEventHandler, this, efd));

  } catch (std::exception& e) {
    GLOG_ERROR("failed to init handler " << e.what());
  }
}

int NetworkXioServer::Disk::runEventHandler(int efd) {

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

        NetworkXioClientData *cd =
            reinterpret_cast<NetworkXioClientData *>(pXioReq->pClientData);
        cd->worker_bottom_half(pXioReq);
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

void NetworkXioServer::Disk::stopEventHandler() {
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

void NetworkXioServer::run(std::promise<void> &promise) {
  int xopt = 2;

  XXEnter();

  serviceHandle_ = IOExecFileServiceInit(numCoresForIO_, queueDepthForIO_,
                                         fileTranslatorFunc_, newInstance_);

  if (serviceHandle_ == nullptr) {
    throw std::bad_alloc();
  }

  disk_.startEventHandler(this);

  xio_init();

  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_MAX_IN_IOVLEN, &xopt,
              sizeof(xopt));

  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_MAX_OUT_IOVLEN, &xopt,
              sizeof(xopt));

  xopt = 0;
  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_ENABLE_FLOW_CONTROL,
              &xopt, sizeof(xopt));

  xopt = queue_depth;
  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_SND_QUEUE_DEPTH_MSGS,
              &xopt, sizeof(xopt));

  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_RCV_QUEUE_DEPTH_MSGS,
              &xopt, sizeof(xopt));

  xopt = 1;
  xio_set_opt(NULL, XIO_OPTLEVEL_TCP, XIO_OPTNAME_TCP_NO_DELAY,
              &xopt, sizeof(xopt));

  const int polling_time_usec = getenv_with_default("GOBJFS_POLLING_TIME_USEC", POLLING_TIME_USEC_DEFAULT);

  ctx = std::shared_ptr<xio_context>(
      xio_context_create(NULL, polling_time_usec, -1),
      xio_destroy_ctx_shutdown);

  if (ctx == nullptr) {
    throw FailedCreateXioContext("failed to create XIO context");
  }

  std::string uri = transport_ + "://" + ipaddr_ + ":" + std::to_string(port_);

  xio_session_ops server_ops;
  server_ops.on_session_event = static_on_session_event<NetworkXioServer>;
  server_ops.on_new_session = static_on_new_session<NetworkXioServer>;
  server_ops.on_msg_send_complete = NULL;
  server_ops.on_msg = NULL;
  server_ops.assign_data_in_buf = NULL;
  server_ops.on_msg_error = NULL;

  GLOG_INFO("bind XIO server to '" << uri << "'");
  server = std::shared_ptr<xio_server>(
      xio_bind(ctx.get(), &server_ops, uri.c_str(), NULL, 0, this), xio_unbind);
  if (server == nullptr) {
    throw FailedBindXioServer("failed to bind XIO server");
  }

  try {
    wq_ = std::make_shared<NetworkXioWorkQueue>("ovs_xio_wq", numCoresForIO_);
  } catch (const WorkQueueThreadsException &) {
    GLOG_FATAL("failed to create workqueue thread pool");
    throw;
  } catch (const std::bad_alloc &) {
    GLOG_FATAL("failed to allocate requested storage space for workqueue");
    throw;
  }

  xio_mpool = std::shared_ptr<xio_mempool>(
      xio_mempool_create(-1, XIO_MEMPOOL_FLAG_REG_MR), xio_mempool_destroy);
  if (xio_mpool == nullptr) {
    GLOG_FATAL("failed to create XIO memory pool");
    throw FailedCreateXioMempool("failed to create XIO memory pool");
  }
  (void)xio_mempool_add_slab(xio_mpool.get(), 4096, 0, queue_depth, 32,
                             DirectIOSize);
  (void)xio_mempool_add_slab(xio_mpool.get(), 32768, 0, queue_depth, 32,
                             DirectIOSize);
  (void)xio_mempool_add_slab(xio_mpool.get(), 65536, 0, queue_depth, 32,
                             DirectIOSize);
  (void)xio_mempool_add_slab(xio_mpool.get(), 131072, 0, 256, 32, DirectIOSize);
  (void)xio_mempool_add_slab(xio_mpool.get(), 1048576, 0, 32, 4, DirectIOSize);


  // create multiple ports for load-balancing
  // tie a thread to each port
  // then xio_accept() will decide which port+thread to use for a new connection
  for (int i = 0; i < MAX_PORTAL_THREADS; i++) {
    std::string uri = transport_ + "://" + ipaddr_ + ":" + std::to_string(port_ + i + 1);
    cd_[i].ncd_uri = uri;
    cd_[i].ncd_server = this;
    cd_[i].ncd_thread = std::thread(portal_func, &cd_[i]);
  }

  promise.set_value();

  while (not stopping) {
    int ret = xio_context_run_loop(ctx.get(), XIO_INFINITE);
    // VERIFY(ret == 0);
    assert(ret == 0);
  }

  for (int i = 0; i < MAX_PORTAL_THREADS; i++) {
    cd_[i].ncd_thread.join();
  }

  server.reset();
  ctx.reset();
  xio_mpool.reset();

  disk_.stopEventHandler();

  if (serviceHandle_) {
    IOExecFileServiceDestroy(serviceHandle_);
    serviceHandle_ = nullptr;
  }

  std::lock_guard<std::mutex> lock_(mutex_);
  stopped = true;
  cv_.notify_one();

  XXExit();
}

int
NetworkXioServer::create_session_connection(xio_session *session,
                                            xio_session_event_data *evdata,
                                            NetworkXioClientData* cd) {
  try {
    
    cd->ncd_session = session;
    cd->ncd_conn = evdata->conn;
    cd->ncd_disconnected = false;
    cd->ncd_refcnt = 0;
    cd->ncd_mpool = xio_mpool.get();
    cd->ncd_server = this;

    cd->ncd_ioh = new NetworkXioIOHandler(serviceHandle_, disk_.eventHandle_, wq_);

  } catch (...) {

    GLOG_ERROR("cannot create client data or IO handler");
    if (cd) { 
      delete cd;
    }
    return -1;

  }

  // automatically done with portal_ops separate from server_ops
  // xio_connection_attr xconattr;
  // xconattr.user_context = cd;
  // (void)xio_modify_connection(evdata->conn, &xconattr,
  //                          XIO_CONNECTION_ATTR_USER_CTX);
  return 0;
}

void NetworkXioServer::destroy_client_data(NetworkXioClientData* cd) {
  if (cd->ncd_disconnected && !cd->ncd_refcnt) {
    xio_connection_destroy(cd->ncd_conn);
    delete cd->ncd_ioh;
  }
}

void NetworkXioServer::destroy_session_connection(
    xio_session *session ATTRIBUTE_UNUSED, 
    xio_session_event_data *evdata,
    NetworkXioClientData* cd) {
  //auto cd = static_cast<NetworkXioClientData *>(evdata->conn_user_context);
  if (cd) {
    cd->ncd_disconnected = true;
    cd->ncd_conn = nullptr;
    destroy_client_data(cd);
  }
}

int NetworkXioServer::on_new_session(xio_session *session,
                                     xio_new_session_req * /*req*/) {

  const char* portal_array[MAX_PORTAL_THREADS];

  // tell xio_accept which portals are available
  for (int i = 0; i < MAX_PORTAL_THREADS; i++) {
    portal_array[i] = cd_[i].ncd_uri.c_str();
  }

  if (xio_accept(session, portal_array, MAX_PORTAL_THREADS, NULL, 0) < 0) {
    GLOG_ERROR(
        "cannot accept new session, error: " << xio_strerror(xio_errno()));
  }

  GLOG_DEBUG("Got a new connection request");
  return 0;
}

int NetworkXioServer::on_session_event(xio_session *session,
                                       xio_session_event_data *event_data,
                                       NetworkXioClientData* cd) {

  switch (event_data->event) {
  case XIO_SESSION_NEW_CONNECTION_EVENT:
    // signals a new connection for existing session 
    GLOG_INFO("Received XIO_SESSION_NEW_CONNECTION_EVENT " << gettid());
    if (cd) {
      create_session_connection(session, event_data, cd);
    }
    break;
  case XIO_SESSION_CONNECTION_TEARDOWN_EVENT:
    // signals loss of a connection within existing session
    GLOG_INFO("Received XIO_SESSION_CONNECTION_TEARDOWN_EVENT " << gettid());
    if (cd) {
      destroy_session_connection(session, event_data, cd);
    }
    break;
  case XIO_SESSION_TEARDOWN_EVENT:
    // signals loss of session
    GLOG_INFO("Received XIO_SESSION_TEARDOWN_EVENT " << gettid());
    xio_session_destroy(session);
    for (int i = 0; i < MAX_PORTAL_THREADS; i++) {
      xio_context_stop_loop(cd_[i].ncd_ctx);
    }
    xio_context_stop_loop(ctx.get());
    break;
  default:
    GLOG_INFO("Received unknown event ");
    break;
  };
  XXExit();
  return 0;
}

void NetworkXioServer::deallocate_request(NetworkXioRequest *req) {

  if ((req->op == NetworkXioMsgOpcode::ReadRsp) && req->data) {
    if (req->from_pool) {
      xio_mempool_free(&req->reg_mem);
    } else {
      xio_mem_free(&req->reg_mem);
    }
  }

  NetworkXioClientData *cd = req->pClientData;
  cd->ncd_refcnt--;
  destroy_client_data(cd);

  delete req;
}

int NetworkXioServer::on_msg_send_complete(xio_session *session
                                               ATTRIBUTE_UNUSED,
                                           xio_msg *msg ATTRIBUTE_UNUSED,
                                           void *cb_user_ctx) {
  NetworkXioClientData *clientData =
      static_cast<NetworkXioClientData *>(cb_user_ctx);
  NetworkXioRequest *req = clientData->ncd_done_reqs.front();
  clientData->ncd_done_reqs.pop_front();
  deallocate_request(req);
  return 0;
}

void NetworkXioServer::send_reply(NetworkXioRequest *req) {
  XXEnter();
  xio_msg *xio_req = req->xio_req;

  memset(&req->xio_reply, 0, sizeof(xio_msg));

  vmsg_sglist_set_nents(&req->xio_req->in, 0);
  xio_req->in.header.iov_base = NULL;
  xio_req->in.header.iov_len = 0;
  req->xio_reply.request = xio_req;

  req->xio_reply.out.header.iov_base =
      const_cast<void *>(reinterpret_cast<const void *>(req->s_msg.c_str()));
  req->xio_reply.out.header.iov_len = req->s_msg.length();
  if ((req->op == NetworkXioMsgOpcode::ReadRsp) && req->data) {
    vmsg_sglist_set_nents(&req->xio_reply.out, 1);
    req->xio_reply.out.sgl_type = XIO_SGL_TYPE_IOV;
    req->xio_reply.out.data_iov.max_nents = XIO_IOVLEN;
    req->xio_reply.out.data_iov.sglist[0].iov_base = req->data;
    req->xio_reply.out.data_iov.sglist[0].iov_len = req->data_len;
    req->xio_reply.out.data_iov.sglist[0].mr = req->reg_mem.mr;
  }
  req->xio_reply.flags = XIO_MSG_FLAG_IMM_SEND_COMP;

  int ret = xio_send_response(&req->xio_reply);
  if (ret != 0) {
    GLOG_ERROR("failed to send reply: " << xio_strerror(xio_errno()));
    deallocate_request(req);
  } else {
    req->pClientData->ncd_done_reqs.push_back(req);
  }
  XXExit();
}

int NetworkXioServer::on_request(xio_session *session ATTRIBUTE_UNUSED,
                                 xio_msg *xio_req,
                                 int last_in_rxq ATTRIBUTE_UNUSED,
                                 void *cb_user_ctx) {

  try {
    auto clientData = static_cast<NetworkXioClientData *>(cb_user_ctx);
    NetworkXioRequest *req = new NetworkXioRequest(xio_req, clientData, this);
    req->from_pool = true;
    req->pClientData->ncd_refcnt ++;
    clientData->ncd_ioh->handle_request(req);
  } catch (const std::bad_alloc &) {
    int ret = xio_cancel(xio_req, XIO_E_MSG_CANCELED);
    GLOG_ERROR("failed to allocate request, cancelling XIO request: " << ret);
  }
  return 0;
}

void NetworkXioServer::shutdown() {
  XXEnter();
  if (not stopped) {
    wq_->shutdown();
    stopping = true;
    // TODO STOP all portal loops ?
    xio_context_stop_loop(ctx.get());
    {
      std::unique_lock<std::mutex> lock_(mutex_);
      cv_.wait(lock_, [&] { return stopped == true; });
    }
  }
  XXExit();
}
}
} // namespace
