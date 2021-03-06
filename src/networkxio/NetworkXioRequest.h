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
#pragma once

#include <libxio.h>
#include <functional>
#include <networkxio/NetworkXioCommon.h>

#include <list>
namespace gobjfs {
namespace xio {

struct Work;

typedef std::function<bool(Work *)> workitem_func_t;

struct Work {
  workitem_func_t func;
  void *obj{nullptr};
};

struct NetworkXioClientData;

struct NetworkXioRequest {
  NetworkXioMsgOpcode op{NetworkXioMsgOpcode::Noop};

  void *req_wq{nullptr};

  void *data{nullptr};
  unsigned int data_len{0}; // DataLen of buffer pointed by data
  size_t size{0};           // Size to be written/read.
  uint64_t offset{0};       // at which offset

  ssize_t retval{0};
  int errval{0};
  uintptr_t opaque{0};

  Work work;

  xio_msg *xio_req{nullptr};
  xio_msg xio_reply;
  xio_reg_mem reg_mem;
  bool from_pool{false};

  NetworkXioClientData *pClientData{nullptr};

  void *private_data{nullptr};

  std::string s_msg;
};

class NetworkXioServer;
class NetworkXioIOHandler;

struct NetworkXioClientData {
  xio_session *ncd_session{nullptr};
  xio_connection *ncd_conn{nullptr};
  xio_mempool *ncd_mpool{nullptr};
  std::atomic<bool> ncd_disconnected{false};
  std::atomic<uint64_t> ncd_refcnt{0};
  NetworkXioServer *ncd_server{nullptr};
  NetworkXioIOHandler *ncd_ioh{nullptr};
  std::list<NetworkXioRequest *> ncd_done_reqs;
};
}
} // namespace
