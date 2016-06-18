#include "FileDistributor.h"

#include <glog/logging.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ftw.h> // ftw dir traversal
#include <unistd.h>
#include <boost/program_options.hpp>

namespace gobjfs { 

static constexpr size_t MaxOpenFd = 100;

int32_t 
FileDistributor::initDirectories(const Config& config, bool createFlag)
{
  if (config.mountPoints_.size() == 0) {
    LOG(ERROR) << "no directories specified";
    return -EINVAL;
  }

  if (config.slots_ == 0) {
    LOG(ERROR) << "no slots specified";
    return -EINVAL;
  }

  config_ = config;

  size_t slotsPerMountPoint = config_.slots_/(config_.mountPoints_.size());

  int ret = 0;

  for (auto& mnt : config_.mountPoints_)
  {
    for (size_t idx = 0; idx < slotsPerMountPoint; idx ++)
    {
      auto dir = mnt + "/dir" + std::to_string(idx) + "/";
      if (createFlag) 
      {
        ret = mkdir(dir.c_str(), S_IRWXU | S_IRWXO | S_IRWXG);
        if (ret != 0) {
          LOG(ERROR) << "failed to create dir=" << dir << " errno=" << errno;
          break;
        }
      }
      dirs_.push_back(dir);
    }
    if (ret != 0) {
      LOG(ERROR) << "Failed to initialize mountpoint=" << mnt;
      break;
    }
  }

  if (ret == 0) {
    if (dirs_.size() != config_.slots_) {
      LOG(ERROR) << "dir size=" << dirs_.size() << " not equal to expected=" << config_.slots_;
      ret = -ENOTDIR;
    }
  }

  return ret;
}

const std::string&
FileDistributor::getDir(const std::string& fileName) const
{
  static std::hash<std::string> hasher;
  auto slot = hasher(fileName) % config_.slots_;
  return dirs_.at(slot);
}

static int 
FileDeleterFunc(const char* fpath, 
  const struct stat* sb,
  int typeflag,
  struct FTW* ftwbuf)
{
  int ret = 0;
  if (typeflag == FTW_F) 
  {
    ret = ::unlink(fpath);
  }
  else if (typeflag == FTW_DP)
  {
    ret = ::rmdir(fpath);
    VLOG(2) << "deleting dir=" << fpath << std::endl;
  }
  if (ret == 0)
  {
    return FTW_CONTINUE; 
  }
  LOG(ERROR) << "stopping.  Failed to delete " << fpath << std::endl;
  return FTW_STOP;
}

int32_t 
FileDistributor::removeDirectories(
  const std::vector<std::string>& mountPoints)
{
  const int numOpenFD = MaxOpenFd;
  int ret = 0;

  for (auto dir : mountPoints)
  {
    // specified depth-first search FTW_DEPTH
    // because we can't delete dir unless
    // files in it are deleted
    ret |= nftw(dir.c_str(),
      FileDeleterFunc, 
      numOpenFD, 
      FTW_DEPTH | FTW_MOUNT | FTW_PHYS);
  
    // WORKAROUND for top-level dir (aka mountpoint) doesn't get removed
    if (ret == FTW_STOP) 
    {
      ret = 0;
    }
    if (ret == 0)
    {
      LOG(INFO) << "removed all dirs and files under path=" << dir;
    }
  }
  return ret;
}

namespace po = boost::program_options;

int32_t
FileDistributor::Config::addOptions(boost::program_options::options_description& desc) {

  po::options_description fileDistribOptions("filedistrib config");

  fileDistribOptions.add_options()
    ("file_distributor.num_dirs", 
      po::value<size_t>(&slots_)->required(),
      " total num of directories to create across mountpoints")
    ("file_distributor.mount_point", 
      po::value<std::vector<std::string>>(&mountPoints_)->multitoken()->required(),
      " mountpoints to manage");

  desc.add(fileDistribOptions);

  return 0;
}

}
