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

#include <util/os_utils.h>
#include <gIOExecFile.h>
#include <gMempool.h>
#include <gobjfs_client.h>
#include <gobjfs_server.h>
#include <networkxio/NetworkXioServer.h>

#include <gobjfs_log.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/types.h>
#include <future>
#include <thread>

using namespace gobjfs::xio;

static int fileTranslatorFunc(const char *old_name, size_t old_length,
                              char *new_name);

class NetworkXioServerTest : public testing::Test {

  int fd{-1};

public:
  char configFile[512];

  gobjfs_xio_server_handle xs;
  std::future<void> fut;
  int portNumber{21321};

  int testDataFd{gobjfs::os::FD_INVALID};
  static const std::string testDataFilePath;
  static const std::string testDataFileName;
  static const std::string testDataFileFullName;

  NetworkXioServerTest() {}

  virtual void SetUp() override {

    strcpy(configFile, "ioexecfiletestXXXXXX");

    fd = mkstemp(configFile);

    const char *configContents = "[ioexec]\n"
                                 "ctx_queue_depth=200\n"
                                 "cpu_core=0\n";

    ssize_t writeSz = write(fd, configContents, strlen(configContents));

    EXPECT_EQ(writeSz, strlen(configContents));

    bool newInstance = true;
    xs = gobjfs_xio_server_start("tcp", "127.0.0.1", portNumber, 1, 1, 200,
                                 fileTranslatorFunc, newInstance);
    EXPECT_NE(xs, nullptr);
  }

  virtual void TearDown() override {

    int ret = 0;
    ret = gobjfs_xio_server_stop(xs);
    EXPECT_EQ(ret, 0);

    {
      ret = close(fd);
      ASSERT_EQ(ret, 0);
      ret = ::unlink(configFile);
      ASSERT_EQ(ret, 0);
    }
  }

  void createDataFile() {

    int fd = creat(testDataFileFullName.c_str(), S_IRUSR | S_IWUSR);
    EXPECT_GE(fd, 0);

    const size_t bufSize = 65536 * 1024;
    char* buf = (char*)malloc(bufSize);
    memset(buf, 'a', bufSize);

    ssize_t ret = write(fd, buf, bufSize);
    EXPECT_EQ(ret, bufSize);

    free(buf);

    ret = close(fd);
    EXPECT_EQ(ret, 0);
  }

  void removeDataFile(bool check = true) {

    int ret = ::unlink(testDataFileFullName.c_str());
    if (check)
      ASSERT_EQ(ret, 0);
  }

  virtual ~NetworkXioServerTest() {}
};

const std::string NetworkXioServerTest::testDataFilePath = "/tmp/";
const std::string NetworkXioServerTest::testDataFileName = "abcd";
const std::string NetworkXioServerTest::testDataFileFullName =
    std::string(NetworkXioServerTest::testDataFilePath) +
    std::string(NetworkXioServerTest::testDataFileName);

int fileTranslatorFunc(const char *old_name, size_t old_length,
                       char *new_name) {
  strcpy(new_name, NetworkXioServerTest::testDataFilePath.c_str());
  strncat(new_name, old_name, old_length);
  return 0;
}

TEST_F(NetworkXioServerTest, MultipleClients) {

  auto ctx_attr = ctx_attr_new();

  ctx_attr_set_transport(ctx_attr, "tcp", "127.0.0.1", portNumber);

  std::vector<client_ctx_ptr> ptr_vec;

  for (int i = 0; i < 100; i++) {
    auto ctx = ctx_new(ctx_attr);
    EXPECT_NE(ctx, nullptr);

    int err = ctx_init(ctx);
    EXPECT_EQ(err, 0);

    ptr_vec.push_back(ctx);
  }

  for (auto &ctx_ptr : ptr_vec) {
    // disconnect the client from server
    auto stats_string = ctx_get_stats(ctx_ptr);
    EXPECT_NE(stats_string.find("num_failed=0"), std::string::npos);
    ctx_ptr.reset();
  }
}

TEST_F(NetworkXioServerTest, FileDoesntExist) {

  removeDataFile(false); // file may not exist; dont fail

  static constexpr size_t BufferSize = gobjfs::os::DirectIOSize;
  // shorten read size to test unaligned reads
  static constexpr size_t ShortenSize = 10;

  size_t times = 100;

  auto ctx_attr = ctx_attr_new();

  ctx_attr_set_transport(ctx_attr, "tcp", "127.0.0.1", portNumber);

  client_ctx_ptr ctx = ctx_new(ctx_attr);
  EXPECT_NE(ctx, nullptr);

  int err = ctx_init(ctx);
  EXPECT_EQ(err, 0);

  for (decltype(times) i = 0; i < times; i++) {

    auto rbuf = (char *)malloc((i + 1) * BufferSize);
    EXPECT_NE(rbuf, nullptr);

    size_t readSz = BufferSize - ShortenSize;

    auto sz =
        gobjfs::xio::read(ctx, testDataFileName, rbuf, readSz, i * BufferSize);

    // reads will fail since file doesnt exist
    EXPECT_LT(sz, 0);

    free(rbuf);
  }

  auto stats_string = ctx_get_stats(ctx);
  auto expected_str = "num_failed=" + std::to_string(times);
  EXPECT_NE(stats_string.find(expected_str), std::string::npos);
  ctx.reset();
}

TEST_F(NetworkXioServerTest, AsyncFileDoesntExist) {

  removeDataFile(false); // file may not exist; dont fail

  static constexpr size_t BufferSize = gobjfs::os::DirectIOSize;
  // shorten read size to test unaligned reads
  static constexpr size_t ShortenSize = 10;

  size_t times = MAX_AIO_BATCH_SIZE;

  auto ctx_attr = ctx_attr_new();

  ctx_attr_set_transport(ctx_attr, "tcp", "127.0.0.1", portNumber);

  client_ctx_ptr ctx = ctx_new(ctx_attr);
  EXPECT_NE(ctx, nullptr);

  int err = ctx_init(ctx);
  EXPECT_EQ(err, 0);
  EXPECT_EQ(ctx_is_disconnected(ctx, 0), false);

  std::vector<giocb *> iocb_vec;

  size_t readSz = BufferSize - ShortenSize;

  std::string zeroString;

  for (decltype(times) i = 0; i < times; i++) {

    auto rbuf = (char *)malloc(BufferSize);
    EXPECT_NE(rbuf, nullptr);

    giocb *iocb = new giocb;
    iocb->filename = testDataFileName;
    iocb->aio_buf = rbuf;
    iocb->aio_offset = i * BufferSize;
    iocb->aio_nbytes = readSz;

    iocb_vec.push_back(iocb);
  }

  auto ret = aio_readv(ctx, iocb_vec);
  EXPECT_EQ(ret, 0);

  ret = aio_suspendv(ctx, iocb_vec, nullptr);
  EXPECT_EQ(ret, -1);

  for (auto iocb : iocb_vec) {
    auto retcode = aio_return(ctx, iocb);
    EXPECT_EQ(retcode, -EIO);
    aio_finish(ctx, iocb);
    free(iocb->aio_buf);
    delete iocb;
  }

  auto stats_string = ctx_get_stats(ctx);
  auto expected_str = "num_failed=" + std::to_string(times);
  EXPECT_NE(stats_string.find(expected_str), std::string::npos);

  ctx.reset();
  EXPECT_EQ(ctx_is_disconnected(ctx, 0), true);
}

TEST_F(NetworkXioServerTest, SyncRead) {

  createDataFile();

  static constexpr size_t BufferSize = gobjfs::os::DirectIOSize;
  // shorten read size to test unaligned reads
  static constexpr size_t ShortenSize = 10;

  size_t times = 100;

  auto ctx_attr = ctx_attr_new();

  ctx_attr_set_transport(ctx_attr, "tcp", "127.0.0.1", portNumber);

  client_ctx_ptr ctx = ctx_new(ctx_attr);
  EXPECT_NE(ctx, nullptr);

  int err = ctx_init(ctx);
  EXPECT_EQ(err, 0);

  for (decltype(times) i = 0; i < times; i++) {

    auto rbuf = (char *)malloc(BufferSize);
    EXPECT_NE(rbuf, nullptr);

    size_t readSz = BufferSize - ShortenSize;

    auto sz =
        gobjfs::xio::read(ctx, testDataFileName, rbuf, readSz, i * BufferSize);

    // reads will fail since file doesnt exist
    EXPECT_EQ(sz, readSz);

    free(rbuf);
  }

  removeDataFile();

  auto stats_string = ctx_get_stats(ctx);
  auto expected_str = "num_queued=" + std::to_string(times);
  std::cerr << stats_string << std::endl;
  EXPECT_NE(stats_string.find(expected_str), std::string::npos);
  EXPECT_NE(stats_string.find("num_failed=0"), std::string::npos);

  ctx.reset();
}

TEST_F(NetworkXioServerTest, AsyncRead) {

  createDataFile();

  static constexpr size_t BufferSize = gobjfs::os::DirectIOSize;
  // shorten read size to test unaligned reads
  static constexpr size_t ShortenSize = 10;

  size_t times = 100;

  auto ctx_attr = ctx_attr_new();

  ctx_attr_set_transport(ctx_attr, "tcp", "127.0.0.1", portNumber);

  client_ctx_ptr ctx = ctx_new(ctx_attr);
  EXPECT_NE(ctx, nullptr);

  int err = ctx_init(ctx);
  EXPECT_EQ(err, 0);
  EXPECT_EQ(ctx_is_disconnected(ctx, 0), false);

  std::vector<giocb *> vec;

  size_t readSz = BufferSize - ShortenSize;

  for (decltype(times) i = 0; i < times; i++) {

    auto rbuf = (char *)malloc(BufferSize);
    EXPECT_NE(rbuf, nullptr);

    giocb *iocb = new giocb;
    iocb->filename = testDataFileName;
    iocb->aio_buf = rbuf;
    iocb->aio_offset = i * BufferSize;
    iocb->aio_nbytes = readSz;

    auto ret = aio_read(ctx, iocb);
    EXPECT_EQ(ret, 0);

    vec.push_back(iocb);
  }

  for (auto &iocb : vec) {

    auto ret = aio_suspend(ctx, iocb, nullptr);
    EXPECT_EQ(ret, 0);

    auto retcode = aio_return(ctx, iocb);
    EXPECT_EQ(retcode, readSz);

    aio_finish(ctx, iocb);
    free(iocb->aio_buf);
    delete iocb;
  }

  removeDataFile();

  auto stats_string = ctx_get_stats(ctx);
  auto expected_str = "num_queued=" + std::to_string(times);
  EXPECT_NE(stats_string.find(expected_str), std::string::npos);
  EXPECT_NE(stats_string.find("num_failed=0"), std::string::npos);

  ctx.reset();
  EXPECT_EQ(ctx_is_disconnected(ctx, 0), true);
}

TEST_F(NetworkXioServerTest, MultiAsyncRead) {

  createDataFile();

  static constexpr size_t BufferSize = gobjfs::os::DirectIOSize;
  // shorten read size to test unaligned reads
  static constexpr size_t ShortenSize = 10;

  size_t times = MAX_AIO_BATCH_SIZE;

  auto ctx_attr = ctx_attr_new();

  ctx_attr_set_transport(ctx_attr, "tcp", "127.0.0.1", portNumber);

  client_ctx_ptr ctx = ctx_new(ctx_attr);
  EXPECT_NE(ctx, nullptr);

  int err = ctx_init(ctx);
  EXPECT_EQ(err, 0);

  std::vector<giocb *> iocb_vec;
  std::vector<std::string> filename_vec;

  size_t readSz = BufferSize - ShortenSize;

  for (decltype(times) i = 0; i < times; i++) {

    auto rbuf = (char *)malloc(BufferSize);
    EXPECT_NE(rbuf, nullptr);

    giocb *iocb = new giocb;
    iocb->filename = testDataFileName;
    iocb->aio_buf = rbuf;
    iocb->aio_offset = i * BufferSize;
    iocb->aio_nbytes = readSz;

    iocb_vec.push_back(iocb);
  }

  auto ret = aio_readv(ctx, iocb_vec);
  EXPECT_EQ(ret, 0);

  ret = aio_suspendv(ctx, iocb_vec, nullptr);
  EXPECT_EQ(ret, 0);

  for (auto &iocb : iocb_vec) {
    auto retcode = aio_return(ctx, iocb);
    EXPECT_EQ(retcode, readSz);
    aio_finish(ctx, iocb);
    free(iocb->aio_buf);
    delete iocb;
  }

  removeDataFile();

  auto stats_string = ctx_get_stats(ctx);
  auto expected_str = "num_queued=" + std::to_string(times);
  EXPECT_NE(stats_string.find(expected_str), std::string::npos);
  EXPECT_NE(stats_string.find("num_failed=0"), std::string::npos);

  ctx.reset();
  EXPECT_EQ(ctx_is_disconnected(ctx, 0), true);
}

TEST_F(NetworkXioServerTest, CheckConnection) {

  auto ctx_attr = ctx_attr_new();

  ctx_attr_set_transport(ctx_attr, "tcp", "127.0.0.1", portNumber);

  client_ctx_ptr ctx = ctx_new(ctx_attr);
  EXPECT_NE(ctx, nullptr);

  int err = ctx_init(ctx);
  EXPECT_EQ(err, 0);

  EXPECT_EQ(ctx_is_disconnected(ctx, 0), false);

  ctx.reset();

  EXPECT_EQ(ctx_is_disconnected(ctx, 0), true);
}
