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
#include <assert.h>
#include <iostream>
#include <fcntl.h>
#include <string.h>
#include <vector>

#include <gobjfs_client.h>
#include <networkxio/gobjfs_client_common.h> // GLOG_DEBUG

int times = 10;
int numports = 1;

using namespace gobjfs::xio;

void NetworkServerWriteReadTest() {

  std::vector<gobjfs::xio::client_ctx_attr_ptr> ctx_attr_vec;

  for (int idx = 0; idx < numports; idx ++) {
    auto ctx_attr = ctx_attr_new();
    ctx_attr_set_transport(ctx_attr, "tcp", "127.0.0.1", 21321 + (100 * idx));
    ctx_attr_vec.push_back(ctx_attr);
  }

  // open connections 
  client_ctx_ptr ctx = ctx_new(ctx_attr_vec);
  assert(ctx != nullptr);

  int err = ctx_init(ctx);
  if (err < 0) {
    GLOG_ERROR("Volume open failed ");
    return;
  }

  for (int32_t uri_slot = 0; uri_slot < ctx_attr_vec.size(); uri_slot ++) {

    std::vector<giocb *> iocb_vec;
  
    for (int i = 0; i < times; i++) {
  
      auto rbuf = (char *)malloc(4096);
      assert(rbuf != nullptr);
  
      giocb *iocb = new giocb;
      iocb->filename = "abcd";
      iocb->aio_buf = rbuf;
      iocb->aio_offset = times * 4096;
      iocb->aio_nbytes = 4096;
  
      iocb_vec.push_back(iocb);
    }
  
    auto ret = aio_readv(ctx, iocb_vec, uri_slot);
  
    if (ret == 0) {
      ret = aio_suspendv(ctx, iocb_vec, nullptr);
    }
  
    for (auto &elem : iocb_vec) {
      aio_finish(ctx, elem);
      free(elem->aio_buf);
      delete elem;
    }
  }

  std::cout << ctx_get_stats(ctx) << std::endl;
}

int main(int argc, char *argv[]) {

  if (argc > 1) {
    times = atoi(argv[1]);
  }

  NetworkServerWriteReadTest();
}
