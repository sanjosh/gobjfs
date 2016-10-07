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

#include "TimerNotifier.h"

#include <cassert>       // assert
#include <gobjfs_log.h>  // LOG
#include <strings.h>     // bzero
#include <sys/epoll.h>   // epoll
#include <sys/timerfd.h> // timerfd
#include <unistd.h>      // read

namespace gobjfs {
namespace os {

TimerNotifier::TimerNotifier(int timeoutSec, int timeoutNanosec) {

  int ret = 0;

  // set to nonblock because fd will be passed to epoll
  fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (fd_ < 0) {
    fd_ = -1;
    ret = -errno;
    LOG(ERROR) << "Failed to create timerfd errno=" << ret;
  }

  struct itimerspec new_value;
  // gone in sixty seconds
  new_value.it_value.tv_sec = 60;
  new_value.it_value.tv_nsec = 0;
  new_value.it_interval.tv_sec = timeoutSec;
  new_value.it_interval.tv_nsec = timeoutNanosec;

  ret = timerfd_settime(fd_, 0, &new_value, NULL);
  if (ret < 0) {
    ret = -errno;
    LOG(ERROR) << "Failed to set time timerfd errno=" << ret;
  }

  (void) ret;
}

int TimerNotifier::getFD() {
  return fd_;
} 

int32_t TimerNotifier::recv(uint64_t& count) {
  int ret = 0;

  ssize_t readSize = read(fd_, &count, sizeof(count));

  if (readSize != sizeof(count)) {

    count = 0;

    if (readSize == -1)
      ret = -errno;
    else
      ret = -EINVAL;

    LOG(ERROR) << "failed to read fd=" << fd_ << " readSize=" << readSize
               << " expectedSize=" << sizeof(count) << " errno=" << ret;
  }
  return ret;
}

int32_t TimerNotifier::destroy() {

  int ret = 0;
  if (fd_ != -1) {
    ret = ::close(fd_);
    if (ret < 0) {
      LOG(ERROR) << "failed to close fd=" << fd_ << " errno=" << -errno;
    } else {
      fd_ = -1;
    }
  }
  return ret;
}

TimerNotifier::~TimerNotifier() { 
  this->destroy(); 
}

}
}
