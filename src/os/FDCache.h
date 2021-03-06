// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 Inktank Storage, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_FDCACHE_H
#define CEPH_FDCACHE_H

#include <memory>
#include <errno.h>
#include <cstdio>
#include "common/hobject.h"
#include "common/Mutex.h"
#include "common/Cond.h"
#include "common/shared_cache.hpp"
#include "common/random_cache.hpp"
#include "include/compat.h"
#include "include/intarith.h"


/**
 * FD Cache
 */
class FDCache : public md_config_obs_t {
public:
  /**
   * FD
   *
   * Wrapper for an fd.  Destructor closes the fd.
   */
  class FD {
  public:
    const int fd;
    atomic_t aio;
    atomic_t truncate;
    Mutex lock;
    Cond cond;;
    FD(int _fd) : fd(_fd), lock("FD::lock") {
      assert(_fd >= 0);
    }
    int operator*() const {
      return fd;
    }
    bool has_truncate() {
      Mutex::Locker l(lock);
      return truncate.read() > 0;
    }
    bool has_aio() {
      Mutex::Locker l(lock);
      return aio.read() > 0;
    }
    void flush() {
      lock.Lock();
      while (aio.read() != 0) {
        cond.Wait(lock);
      }
      lock.Unlock();
    }
    ~FD() {
      assert(truncate.read() == 0);
      assert(aio.read() == 0);
      VOID_TEMP_FAILURE_RETRY(::close(fd));
    }
  };
  typedef ceph::shared_ptr<FD> FDRef;
private:
  CephContext *cct;
  const int registry_shards;
  SharedLRU<ghobject_t, FD> *registry;
  RandomCache<ghobject_t, FDRef> random_cache;
  bool random;

public:
  FDCache(CephContext *cct) : cct(cct),
  registry_shards(cct->_conf->filestore_fd_cache_shards) {
    assert(cct);
    cct->_conf->add_observer(this);
    if (cct->_conf->filestore_fd_cache_random) {
      random_cache.set_size(cct->_conf->filestore_fd_cache_size);
      random = true;
      registry = NULL;
    } else {
      registry = new SharedLRU<ghobject_t, FD>[registry_shards];
      for (int i = 0; i < registry_shards; ++i) {
        registry[i].set_cct(cct);
        registry[i].set_size(
            MAX((cct->_conf->filestore_fd_cache_size / registry_shards), 1));
      }
    }
  }
  ~FDCache() {
    cct->_conf->remove_observer(this);
    if (registry)
      delete[] registry;
  }

  FDRef lookup(const ghobject_t &hoid) {
    if (random) {
      FDRef ret;
      random_cache.lookup(hoid, &ret);
      return ret;
    } else {
      int registry_id = hoid.hobj.get_hash() % registry_shards;
      return registry[registry_id].lookup(hoid);
    }
  }

  FDRef add(const ghobject_t &hoid, int fd, bool *existed) {
    if (random){
      FDRef ret = FDRef(new FD(fd));
      random_cache.add(hoid, ret);
      if (existed)
        *existed = false;
      return ret;
    } else {
      int registry_id = hoid.hobj.get_hash() % registry_shards;
      return registry[registry_id].add(hoid, new FD(fd), existed);
    }
  }

  /// clear cached fd for hoid, subsequent lookups will get an empty FD
  void clear(const ghobject_t &hoid) {
    if (random) {
      random_cache.clear(hoid);
    } else {
      int registry_id = hoid.hobj.get_hash() % registry_shards;
      registry[registry_id].purge(hoid);
    }
  }

  /// md_config_obs_t
  const char** get_tracked_conf_keys() const {
    static const char* KEYS[] = {
      "filestore_fd_cache_size",
      NULL
    };
    return KEYS;
  }
  void handle_conf_change(const md_config_t *conf,
			  const std::set<std::string> &changed) {
    if (changed.count("filestore_fd_cache_size") && !random) {
      for (int i = 0; i < registry_shards; ++i)
        registry[i].set_size(
              MAX((conf->filestore_fd_cache_size / registry_shards), 1));
    }
  }

};
typedef FDCache::FDRef FDRef;

#endif
