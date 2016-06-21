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

#include <gIOExecFile.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <gtest/gtest.h>

#include <util/os_utils.h>
#include <fcntl.h>
#include <sys/types.h>

/*
 * for a null service handle,
 * eventFdOpen, FileOpen, FileDelete, ServiceDestroy
 * must fail
 */

TEST(IOExecFile, NoInitDone) {
  IOExecServiceHandle serviceHandle = nullptr;

  int32_t ret;

  auto evHandle = IOExecEventFdOpen(serviceHandle);
  EXPECT_EQ(evHandle, nullptr);

  auto fd = IOExecEventFdGetReadFd(evHandle);
  EXPECT_EQ(fd, gobjfs::os::FD_INVALID);

  auto handle = IOExecFileOpen(serviceHandle, "/tmp/abc", O_RDWR | O_CREAT);
  EXPECT_EQ(handle, nullptr);

  ret = IOExecFileTruncate(handle, 512);
  EXPECT_NE(ret, 0);

  ret = IOExecFileDelete(serviceHandle, "/tmp/abc", 0, evHandle);
  EXPECT_NE(ret, 0);

  ret = IOExecFileServiceDestroy(serviceHandle);
  EXPECT_NE(ret, 0);
}

class IOExecFileInitTest : public testing::Test {
  
  int configFileFd {-1};


public:
  char configFile[512];

  IOExecFileInitTest() {
  }

  virtual void SetUp() override {
    strcpy(configFile,  "ioexecfiletestXXXXXX");

    configFileFd = mkstemp(configFile);

    const char* configContents = 
      "[ioexec]\n"
      "ctx_queue_depth=200\n"
      "cpu_core=0\n"
      "[file_distributor]\n"
      "mount_point=/tmp/ioexectest\n"
      "num_dirs=3\n"
      ;

    ssize_t writeSz = write(configFileFd, configContents, strlen(configContents));

    EXPECT_EQ(writeSz, strlen(configContents));
  }

  virtual void TearDown() override {
    close(configFileFd);
    int ret = ::unlink(configFile);
    assert(ret == 0);
  }

  virtual ~IOExecFileInitTest() {
  }
};

TEST_F(IOExecFileInitTest, CheckStats) {

  auto serviceHandle = IOExecFileServiceInit(configFile, true);

  uint32_t len = 8192;
  char buffer [len];
  auto ret = IOExecGetStats(serviceHandle, buffer, len);

  EXPECT_LE(ret, len);
  char* found = strstr(buffer, "stats");
  EXPECT_NE(found, nullptr);
  // TODO more sophisticated testing possible
  // can check if buffer is json formatted here
  LOG(INFO) << "len=" << ret << " buf=" << buffer;
}
