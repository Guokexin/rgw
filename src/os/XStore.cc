// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */
#include "include/int_types.h"

#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>

#if defined(__linux__)
#include <linux/fs.h>
#endif

#include <iostream>
#include <map>

#include "include/compat.h"
#include "include/linux_fiemap.h"

#include "common/xattr.h"
#include "chain_xattr.h"

#if defined(DARWIN) || defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/mount.h>
#endif // DARWIN


#include <fstream>
#include <sstream>

#include "XStore.h"
#include "GenericFileStoreBackend.h"
#include "BtrfsFileStoreBackend.h"
#include "XfsFileStoreBackend.h"
#include "ZFSFileStoreBackend.h"
#include "common/BackTrace.h"
#include "include/types.h"
#include "XJournal.h"

#include "osd/osd_types.h"
#include "include/color.h"
#include "include/buffer.h"

#include "common/Timer.h"
#include "common/debug.h"
#include "common/errno.h"
#include "common/run_cmd.h"
#include "common/safe_io.h"
#include "common/perf_counters.h"
#include "common/sync_filesystem.h"
#include "common/fd.h"
#include "HashIndex.h"
#include "DBObjectMap.h"
#include "KeyValueDB.h"

#include "common/ceph_crypto.h"
using ceph::crypto::SHA1;

#include "include/assert.h"

#include "common/config.h"

#ifdef WITH_LTTNG
#include "tracing/objectstore.h"
#else
#define tracepoint(...)
#endif

#define dout_subsys ceph_subsys_xstore
#undef dout_prefix
#define dout_prefix *_dout << "xstore(" << basedir << ") "

#define COMMIT_SNAP_ITEM "snap_%lld"
#define CLUSTER_SNAP_ITEM "clustersnap_%s"

#define REPLAY_GUARD_XATTR "user.cephos.seq"
#define GLOBAL_REPLAY_GUARD_XATTR "user.cephos.gseq"

// XATTR_SPILL_OUT_NAME as a xattr is used to maintain that indicates whether
// xattrs spill over into DBObjectMap, if XATTR_SPILL_OUT_NAME exists in file
// xattrs and the value is "no", it indicates no xattrs in DBObjectMap
#define XATTR_SPILL_OUT_NAME "user.cephos.spill_out"
#define XATTR_NO_SPILL_OUT "0"
#define XATTR_SPILL_OUT "1"

extern void get_attrname(const char *name, char *buf, int len);
extern bool parse_attrname(char **name);

//Initial features in new superblock.
static CompatSet get_fs_initial_compat_set() {
  CompatSet::FeatureSet ceph_osd_feature_compat;
  CompatSet::FeatureSet ceph_osd_feature_ro_compat;
  CompatSet::FeatureSet ceph_osd_feature_incompat;
  return CompatSet(ceph_osd_feature_compat, ceph_osd_feature_ro_compat,
		   ceph_osd_feature_incompat);
}

//Features are added here that this XStore supports.
static CompatSet get_fs_supported_compat_set() {
  CompatSet compat =  get_fs_initial_compat_set();
  //Any features here can be set in code, but not in initial superblock
  compat.incompat.insert(CEPH_FS_FEATURE_INCOMPAT_SHARDS);
  return compat;
}


int XStore::peek_journal_fsid(uuid_d *fsid)
{
  // make sure we don't try to use aio or direct_io (and get annoying
  // error messages from failing to do so); performance implications
  // should be irrelevant for this use
  FileJournal j(*fsid, 0, 0, journalpath.c_str(), false, false);
  return j.peek_fsid(*fsid);
}

void XStore::FSPerfTracker::update_from_perfcounters(
  PerfCounters &logger)
{
  os_commit_latency.consume_next(
    logger.get_tavg_ms(
      l_os_j_lat));
  os_apply_latency.consume_next(
    logger.get_tavg_ms(
      l_os_apply_lat));
}


ostream& operator<<(ostream& out, const XStore::OpSequencer& s)
{
  assert(&out);
  return out << *s.parent;
}

int XStore::get_cdir(const coll_t& cid, char *s, int len) 
{
  const string &cid_str(cid.to_str());
  return snprintf(s, len, "%s/current/%s", basedir.c_str(), cid_str.c_str());
}

int XStore::get_index(const coll_t& cid, Index *index)
{
  int r = index_manager.get_index(cid, basedir, index);
  assert(!m_filestore_fail_eio || r != -EIO);
  return r;
}

int XStore::init_index(const coll_t& cid)
{
  char path[PATH_MAX];
  get_cdir(cid, path, sizeof(path));
  int r = index_manager.init_index(cid, path, target_version);
  assert(!m_filestore_fail_eio || r != -EIO);
  return r;
}

int XStore::lfn_find(const ghobject_t& oid, const Index& index, IndexedPath *path)
{
  IndexedPath path2;
  if (!path)
    path = &path2;
  int r, exist;
  assert(NULL != index.index);
  r = (index.index)->lookup(oid, path, &exist);
  if (r < 0) {
    assert(!m_filestore_fail_eio || r != -EIO);
    return r;
  }
  if (!exist)
    return -ENOENT;
  return 0;
}

int XStore::lfn_truncate(coll_t cid, const ghobject_t& oid, off_t length)
{
  FDRef fd;
  int r = lfn_open(cid, oid, false, &fd);
  if (r < 0)
    return r;
  r = ::ftruncate(**fd, length);
  if (r < 0)
    r = -errno;
  if (r >= 0 && m_filestore_sloppy_crc) {
    int rc = backend->_crc_update_truncate(**fd, length);
    assert(rc >= 0);
  }
  assert(!m_filestore_fail_eio || r != -EIO);
  return r;
}

int XStore::lfn_stat(const coll_t& cid, const ghobject_t& oid, struct stat *buf)
{
  IndexedPath path;
  Index index;
  int r = get_index(cid, &index);
  if (r < 0)
    return r;

  assert(NULL != index.index);
  RWLock::RLocker l((index.index)->access_lock);

  r = lfn_find(oid, index, &path);
  if (r < 0)
    return r;
  r = ::stat(path->path(), buf);
  if (r < 0)
    r = -errno;
  return r;
}

int XStore::lfn_open(const coll_t& cid,
			const ghobject_t& oid,
			bool create,
			FDRef *outfd,
                        Index *index)
{
  assert(get_allow_sharded_objects() ||
	 ( oid.shard_id == shard_id_t::NO_SHARD &&
	   oid.generation == ghobject_t::NO_GEN ));
  assert(outfd);
  int r = 0;
  bool need_lock = true;
  int flags = O_RDWR;

  if (create)
    flags |= O_CREAT;

  Index index2;
  if (!index) {
    index = &index2;
  }
  if (!((*index).index)) {
    r = get_index(cid, index);
    if (r < 0) {
      dout(10) << __func__ << " could not get index r = " << r << dendl;
      return r;
    }
  } else {
    need_lock = false;
  }

  int fd, exist;
  assert(NULL != (*index).index);
  if (need_lock) {
    ((*index).index)->access_lock.get_write();
  }
  if (!replaying) {
    *outfd = fdcache.lookup(oid);
    logger->inc(l_os_fdcache);
    if (*outfd) {
      logger->inc(l_os_fdcache_hit);
      if (need_lock) {
        ((*index).index)->access_lock.put_write();
      }
      return 0;
    }
  }


  IndexedPath path2;
  IndexedPath *path = &path2;
  if (r < 0) {
    derr << "error getting collection index for " << cid
      << ": " << cpp_strerror(-r) << dendl;
    goto fail;
  }
  r = (*index)->lookup(oid, path, &exist);
  if (r < 0) {
    derr << "could not find " << oid << " in index: "
      << cpp_strerror(-r) << dendl;
    goto fail;
  }

  r = ::open((*path)->path(), flags, 0644);
  if (r < 0) {
    r = -errno;
    dout(10) << "error opening file " << (*path)->path() << " with flags="
      << flags << ": " << cpp_strerror(-r) << dendl;
    goto fail;
  }
  fd = r;
  if (create && (!exist)) {
    r = (*index)->created(oid, (*path)->path());
    if (r < 0) {
      VOID_TEMP_FAILURE_RETRY(::close(fd));
      derr << "error creating " << oid << " (" << (*path)->path()
          << ") in index: " << cpp_strerror(-r) << dendl;
      goto fail;
    }
    r = chain_fsetxattr(fd, XATTR_SPILL_OUT_NAME,
                        XATTR_NO_SPILL_OUT, sizeof(XATTR_NO_SPILL_OUT), 1);
    if (r < 0) {
      VOID_TEMP_FAILURE_RETRY(::close(fd));
      derr << "error setting spillout xattr for oid " << oid << " (" << (*path)->path()
                     << "):" << cpp_strerror(-r) << dendl;
      goto fail;
    }
  }

  if (!replaying) {
    bool existed;
    *outfd = fdcache.add(oid, fd, &existed);
    if (existed) {
      TEMP_FAILURE_RETRY(::close(fd));
    }
  } else {
    *outfd = FDRef(new FDCache::FD(fd));
  }

  if (need_lock) {
    ((*index).index)->access_lock.put_write();
  }

  return 0;

 fail:

  if (need_lock) {
    ((*index).index)->access_lock.put_write();
  }

  assert(!m_filestore_fail_eio || r != -EIO);
  return r;
}

void XStore::lfn_close(FDRef fd)
{
}

int XStore::lfn_link(coll_t c, coll_t newcid, const ghobject_t& o, const ghobject_t& newoid)
{
  Index index_new, index_old;
  IndexedPath path_new, path_old;
  int exist;
  int r;
  bool index_same = false;
  if (c < newcid) {
    r = get_index(newcid, &index_new);
    if (r < 0)
      return r;
    r = get_index(c, &index_old);
    if (r < 0)
      return r;
  } else if (c == newcid) {
    r = get_index(c, &index_old);
    if (r < 0)
      return r;
    index_new = index_old;
    index_same = true;
  } else {
    r = get_index(c, &index_old);
    if (r < 0)
      return r;
    r = get_index(newcid, &index_new);
    if (r < 0)
      return r;
  }

  assert(NULL != index_old.index);
  assert(NULL != index_new.index);

  if (!index_same) {

    RWLock::RLocker l1((index_old.index)->access_lock);

    r = index_old->lookup(o, &path_old, &exist);
    if (r < 0) {
      assert(!m_filestore_fail_eio || r != -EIO);
      return r;
    }
    if (!exist)
      return -ENOENT;
  
    RWLock::WLocker l2((index_new.index)->access_lock);

    r = index_new->lookup(newoid, &path_new, &exist);
    if (r < 0) {
      assert(!m_filestore_fail_eio || r != -EIO);
      return r;
    }
    if (exist)
      return -EEXIST;

    dout(25) << "lfn_link path_old: " << path_old << dendl;
    dout(25) << "lfn_link path_new: " << path_new << dendl;
    r = ::link(path_old->path(), path_new->path());
    if (r < 0)
      return -errno;

    r = index_new->created(newoid, path_new->path());
    if (r < 0) {
      assert(!m_filestore_fail_eio || r != -EIO);
      return r;
    }
  } else {
    RWLock::WLocker l1((index_old.index)->access_lock);

    r = index_old->lookup(o, &path_old, &exist);
    if (r < 0) {
      assert(!m_filestore_fail_eio || r != -EIO);
      return r;
    }
    if (!exist)
      return -ENOENT;

    r = index_new->lookup(newoid, &path_new, &exist);
    if (r < 0) {
      assert(!m_filestore_fail_eio || r != -EIO);
      return r;
    }
    if (exist)
      return -EEXIST;

    dout(25) << "lfn_link path_old: " << path_old << dendl;
    dout(25) << "lfn_link path_new: " << path_new << dendl;
    r = ::link(path_old->path(), path_new->path());
    if (r < 0)
      return -errno;

    r = index_new->created(newoid, path_new->path());
    if (r < 0) {
      assert(!m_filestore_fail_eio || r != -EIO);
      return r;
    }
  }    
  return 0;
}

int XStore::lfn_unlink(coll_t cid, const ghobject_t& o,
			  const SequencerPosition &spos,
			  bool force_clear_omap, int osr)
{
  Index index;
  int r = get_index(cid, &index);
  if (r < 0) {
    dout(25) << __func__ << " get_index failed " << cpp_strerror(r) << dendl;
    return r;
  }

  assert(NULL != index.index);
  RWLock::WLocker l((index.index)->access_lock);

  {
    IndexedPath path;
    int exist;
    r = index->lookup(o, &path, &exist);
    if (r < 0) {
      assert(!m_filestore_fail_eio || r != -EIO);
      return r;
    }

    if (!force_clear_omap) {
      struct stat st;
      r = ::stat(path->path(), &st);
      if (r < 0) {
	r = -errno;
	if (r == -ENOENT) {
	  wbthrottles[osr % wbthrottle_num]->clear_object(o); // should be only non-cache ref
	  fdcache.clear(o);
	} else {
	  assert(!m_filestore_fail_eio || r != -EIO);
	}
	dout(25) << __func__ << " stat failed " << cpp_strerror(r) << dendl;
	return r;
      } else if (st.st_nlink == 1) {
	force_clear_omap = true;
      }
    }
    if (force_clear_omap) {
      dout(20) << __func__ << ": clearing omap on " << o
	       << " in cid " << cid << dendl;
      r = object_map->clear(o, &spos);
      if (r < 0 && r != -ENOENT) {
	dout(25) << __func__ << " omap clear failed " << cpp_strerror(r) << dendl;
	assert(!m_filestore_fail_eio || r != -EIO);
	return r;
      }
      if (g_conf->filestore_debug_inject_read_err) {
	debug_obj_on_delete(o);
      }
      wbthrottles[osr % wbthrottle_num]->clear_object(o); // should be only non-cache ref
      fdcache.clear(o);
      if (o.is_pgmeta()) {
        pgmeta_cache.erase_pgmeta_key(o);
      }
    } else {
      /* Ensure that replay of this op doesn't result in the object_map
       * going away.
       */
      if (!backend->can_checkpoint()) {
        if (o.is_pgmeta())
          pgmeta_cache.submit_pgmeta_keys(o);
 	object_map->sync(&o, &spos);
      }
    }
  }
  r = index->unlink(o);
  if (r < 0) {
    dout(25) << __func__ << " index unlink failed " << cpp_strerror(r) << dendl;
    return r;
  }
  return 0;
}

XStore::XStore(const std::string &base, const std::string &jdev, osflagbits_t flags, const char *name, bool do_update) :
  XJournalingObjectStore(base),
  Store(-1, -1, -1, 0, base, g_conf->filestore_sloppy_crc_block_size),
  internal_name(name),
  journalpath(jdev),
  generic_flags(flags),
  fsid_fd(-1),
  backend(NULL),
  index_manager(do_update),
  pgmeta_cache(this, g_conf->filestore_pgmeta_cache_shards,
               g_conf->filestore_pgmeta_cache_shard_bytes),
  lock("XStore::lock"),
  force_sync(false), 
  sync_entry_timeo_lock("sync_entry_timeo_lock"),
  timer(g_ceph_context, sync_entry_timeo_lock),
  stop(false), sync_thread(this),
  jwa_lock("XStore::jwa_lock"),
  jwa_stop(false), jwa_thread(this),
  fdcache(g_ceph_context),
  default_osr("default"),
  next_osr_id(0),
  op_queue_len(0), op_queue_bytes(0),
  op_throttle_lock("XStore::op_throttle_lock"),
  ondisk_finisher_num(g_conf->filestore_ondisk_finisher_threads),
  apply_finisher_num(g_conf->filestore_apply_finisher_threads),
  wbthrottle_num(g_conf->filestore_wbthrottle_num),
  op_tp(g_ceph_context, "XStore::op_tp", g_conf->filestore_op_threads, "filestore_op_threads"),
  op_wq(this, g_conf->filestore_op_thread_timeout,
	g_conf->filestore_op_thread_suicide_timeout, &op_tp),
  logger(NULL),
  read_error_lock("XStore::read_error_lock"),
  m_filestore_commit_timeout(g_conf->filestore_commit_timeout),
  m_filestore_fiemap_threshold(g_conf->filestore_fiemap_threshold),
  m_filestore_max_sync_interval(g_conf->filestore_max_sync_interval),
  m_filestore_min_sync_interval(g_conf->filestore_min_sync_interval),
  m_filestore_fail_eio(g_conf->filestore_fail_eio),
  m_filestore_fadvise(g_conf->filestore_fadvise),
  do_update(do_update),
  m_journal_dio(g_conf->journal_dio),
  m_journal_aio(g_conf->journal_aio),
  m_journal_force_aio(g_conf->journal_force_aio),
  m_osd_rollback_to_cluster_snap(g_conf->osd_rollback_to_cluster_snap),
  m_osd_use_stale_snap(g_conf->osd_use_stale_snap),
  m_filestore_queue_max_ops(g_conf->filestore_queue_max_ops),
  m_filestore_queue_max_bytes(g_conf->filestore_queue_max_bytes),
  m_filestore_queue_committing_max_ops(g_conf->filestore_queue_committing_max_ops),
  m_filestore_queue_committing_max_bytes(g_conf->filestore_queue_committing_max_bytes),
  m_filestore_do_dump(false),
  m_filestore_dump_fmt(true),
  m_filestore_sloppy_crc(g_conf->filestore_sloppy_crc),
  m_filestore_max_alloc_hint_size(g_conf->filestore_max_alloc_hint_size),
  m_fs_type(0),
  m_filestore_max_inline_xattr_size(0),
  m_filestore_max_inline_xattrs(0)
{
  m_filestore_kill_at.set(g_conf->filestore_kill_at);
  for (int i = 0; i < ondisk_finisher_num; ++i) {
    ostringstream oss;
    oss << "filestore-ondisk-" << i;
    Finisher *f = new Finisher(g_ceph_context, oss.str());
    ondisk_finishers.push_back(f);
  }
  for (int i = 0; i < apply_finisher_num; ++i) {
    ostringstream oss;
    oss << "filestore-apply-" << i;
    Finisher *f = new Finisher(g_ceph_context, oss.str());
    apply_finishers.push_back(f);
  }
  for (int i = 0; i < wbthrottle_num; ++i) {
    ostringstream oss;
    oss << i;
    wbthrottles.push_back(new WBThrottle(g_ceph_context, oss.str()));
  }
  ostringstream oss;
  oss << basedir << "/current";
  current_fn = oss.str();

  ostringstream sss;
  sss << basedir << "/current/commit_op_seq";
  current_op_seq_fn = sss.str();

  ostringstream omss;
  omss << basedir << "/current/omap";
  omap_dir = omss.str();

  // initialize logger
  PerfCountersBuilder plb(g_ceph_context, internal_name, l_os_first, l_os_last);

  plb.add_u64(l_os_jq_max_ops, "journal_queue_max_ops");
  plb.add_u64(l_os_jq_ops, "journal_queue_ops");
  plb.add_u64_counter(l_os_j_ops, "journal_ops");
  plb.add_u64(l_os_jq_max_bytes, "journal_queue_max_bytes");
  plb.add_u64(l_os_jq_bytes, "journal_queue_bytes");
  plb.add_u64_counter(l_os_j_bytes, "journal_bytes");
  plb.add_time_avg(l_os_j_lat, "journal_latency");
  plb.add_u64_counter(l_os_j_wr, "journal_wr");
  plb.add_u64_avg(l_os_j_wr_bytes, "journal_wr_bytes");
  plb.add_u64_counter(l_os_omap_cache_shard_flush, "omap_cache_shard_flush");
  plb.add_u64(l_os_oq_max_ops, "op_queue_max_ops");
  plb.add_u64(l_os_oq_ops, "op_queue_ops");
  plb.add_u64_counter(l_os_ops, "ops");
  plb.add_u64(l_os_oq_max_bytes, "op_queue_max_bytes");
  plb.add_u64(l_os_oq_bytes, "op_queue_bytes");
  plb.add_u64_counter(l_os_bytes, "bytes");
  plb.add_time_avg(l_os_apply_lat, "apply_latency");
  plb.add_u64_counter(l_os_fdcache, "fdcache");
  plb.add_u64_counter(l_os_fdcache_hit, "fdcache_hit");
  plb.add_u64(l_os_committing, "committing");

  plb.add_u64_counter(l_os_commit, "commitcycle");
  plb.add_time_avg(l_os_commit_len, "commitcycle_interval");
  plb.add_time_avg(l_os_commit_lat, "commitcycle_latency");
  plb.add_u64_counter(l_os_j_full, "journal_full");
  plb.add_time_avg(l_os_queue_lat, "queue_transaction_latency_avg");

  logger = plb.create_perf_counters();

  g_ceph_context->get_perfcounters_collection()->add(logger);
  g_ceph_context->_conf->add_observer(this);

  superblock.compat_features = get_fs_initial_compat_set();
}

XStore::~XStore()
{
  for (vector<Finisher*>::iterator it = ondisk_finishers.begin(); it != ondisk_finishers.end(); ++it) {
    delete *it;
    *it = NULL;
  }
  for (vector<Finisher*>::iterator it = apply_finishers.begin(); it != apply_finishers.end(); ++it) {
    delete *it;
    *it = NULL;
  }
  for (vector<WBThrottle*>::iterator it = wbthrottles.begin(); it != wbthrottles.end(); ++it) {
    delete *it;
    *it = NULL;
  }
  g_ceph_context->_conf->remove_observer(this);
  g_ceph_context->get_perfcounters_collection()->remove(logger);

  if (journal)
    journal->logger = NULL;
  delete logger;

  if (m_filestore_do_dump) {
    dump_stop();
  }
}

void XStore::collect_metadata(map<string,string> *pm)
{
  (*pm)["filestore_backend"] = backend->get_name();
  ostringstream ss;
  ss << "0x" << std::hex << m_fs_type << std::dec;
  (*pm)["filestore_f_type"] = ss.str();
}

int XStore::statfs(struct statfs *buf)
{
  if (::statfs(basedir.c_str(), buf) < 0) {
    int r = -errno;
    assert(!m_filestore_fail_eio || r != -EIO);
    return r;
  }
  return 0;
}


int XStore::open_journal()
{
  if (journalpath.length()) {
    dout(10) << "open_journal at " << journalpath << dendl;
    journal = new FileJournal(fsid, &finisher, &sync_cond, journalpath.c_str(),
			      m_journal_dio, m_journal_aio, m_journal_force_aio);
    if (journal)
      journal->logger = logger;
  }
  return 0;
}

int XStore::dump_journal(ostream& out)
{
  int r;

  if (!journalpath.length())
    return -EINVAL;

  FileJournal *journal = new FileJournal(fsid, &finisher, &sync_cond, journalpath.c_str(), m_journal_dio);
  r = journal->dump(out);
  delete journal;
  return r;
}

void XStore::create_backend(long f_type)
{
  m_fs_type = f_type;

  assert(backend == NULL);
  backend = FileStoreBackend::create(f_type, this);

  dout(0) << "backend " << backend->get_name()
	  << " (magic 0x" << std::hex << f_type << std::dec << ")"
	  << dendl;

  switch (f_type) {
#if defined(__linux__)
  case BTRFS_SUPER_MAGIC:
    for (int i = 0; i < wbthrottle_num; ++i) {
      wbthrottles[i]->set_fs(WBThrottle::BTRFS);
    }
    break;

  case XFS_SUPER_MAGIC:
    // wbthrottles is constructed with fs(WBThrottle::XFS)
    break;
#endif
  }

  set_xattr_limits_via_conf();
}

int XStore::mkfs()
{
  int ret = 0;
  char fsid_fn[PATH_MAX];
  uuid_d old_fsid;

  dout(1) << "mkfs in " << basedir << dendl;
  basedir_fd = ::open(basedir.c_str(), O_RDONLY);
  if (basedir_fd < 0) {
    ret = -errno;
    derr << "mkfs failed to open base dir " << basedir << ": " << cpp_strerror(ret) << dendl;
    return ret;
  }

  // open+lock fsid
  snprintf(fsid_fn, sizeof(fsid_fn), "%s/fsid", basedir.c_str());
  fsid_fd = ::open(fsid_fn, O_RDWR|O_CREAT, 0644);
  if (fsid_fd < 0) {
    ret = -errno;
    derr << "mkfs: failed to open " << fsid_fn << ": " << cpp_strerror(ret) << dendl;
    goto close_basedir_fd;
  }

  if (lock_fsid() < 0) {
    ret = -EBUSY;
    goto close_fsid_fd;
  }

  if (read_fsid(fsid_fd, &old_fsid) < 0 || old_fsid.is_zero()) {
    if (fsid.is_zero()) {
      fsid.generate_random();
      dout(1) << "mkfs generated fsid " << fsid << dendl;
    } else {
      dout(1) << "mkfs using provided fsid " << fsid << dendl;
    }

    char fsid_str[40];
    fsid.print(fsid_str);
    strcat(fsid_str, "\n");
    ret = ::ftruncate(fsid_fd, 0);
    if (ret < 0) {
      ret = -errno;
      derr << "mkfs: failed to truncate fsid: "
	   << cpp_strerror(ret) << dendl;
      goto close_fsid_fd;
    }
    ret = safe_write(fsid_fd, fsid_str, strlen(fsid_str));
    if (ret < 0) {
      derr << "mkfs: failed to write fsid: "
	   << cpp_strerror(ret) << dendl;
      goto close_fsid_fd;
    }
    if (::fsync(fsid_fd) < 0) {
      ret = errno;
      derr << "mkfs: close failed: can't write fsid: "
	   << cpp_strerror(ret) << dendl;
      goto close_fsid_fd;
    }
    dout(10) << "mkfs fsid is " << fsid << dendl;
  } else {
    if (!fsid.is_zero() && fsid != old_fsid) {
      derr << "mkfs on-disk fsid " << old_fsid << " != provided " << fsid << dendl;
      ret = -EINVAL;
      goto close_fsid_fd;
    }
    fsid = old_fsid;
    dout(1) << "mkfs fsid is already set to " << fsid << dendl;
  }

  // version stamp
  ret = write_version_stamp();
  if (ret < 0) {
    derr << "mkfs: write_version_stamp() failed: "
	 << cpp_strerror(ret) << dendl;
    goto close_fsid_fd;
  }

  // superblock
  superblock.omap_backend = g_conf->filestore_omap_backend;
  ret = write_superblock();
  if (ret < 0) {
    derr << "mkfs: write_superblock() failed: "
	 << cpp_strerror(ret) << dendl;
    goto close_fsid_fd;
  }

  struct statfs basefs;
  ret = ::fstatfs(basedir_fd, &basefs);
  if (ret < 0) {
    ret = -errno;
    derr << "mkfs cannot fstatfs basedir "
	 << cpp_strerror(ret) << dendl;
    goto close_fsid_fd;
  }

  create_backend(basefs.f_type);

  ret = backend->create_current();
  if (ret < 0) {
    derr << "mkfs: failed to create current/ " << cpp_strerror(ret) << dendl;
    goto close_fsid_fd;
  }

  // write initial op_seq
  {
    uint64_t initial_seq = 0;
    int fd = read_op_seq(&initial_seq);
    if (fd < 0) {
      derr << "mkfs: failed to create " << current_op_seq_fn << ": "
	   << cpp_strerror(fd) << dendl;
      goto close_fsid_fd;
    }
    if (initial_seq == 0) {
      int err = write_op_seq(fd, 1);
      if (err < 0) {
	VOID_TEMP_FAILURE_RETRY(::close(fd));
	derr << "mkfs: failed to write to " << current_op_seq_fn << ": "
	     << cpp_strerror(err) << dendl;
	goto close_fsid_fd;
      }

      if (backend->can_checkpoint()) {
	// create snap_1 too
	current_fd = ::open(current_fn.c_str(), O_RDONLY);
	assert(current_fd >= 0);
	char s[NAME_MAX];
	snprintf(s, sizeof(s), COMMIT_SNAP_ITEM, 1ull);
	ret = backend->create_checkpoint(s, NULL);
	VOID_TEMP_FAILURE_RETRY(::close(current_fd));
	if (ret < 0 && ret != -EEXIST) {
	  VOID_TEMP_FAILURE_RETRY(::close(fd));  
	  derr << "mkfs: failed to create snap_1: " << cpp_strerror(ret) << dendl;
	  goto close_fsid_fd;
	}
      }
    }
    VOID_TEMP_FAILURE_RETRY(::close(fd));  
  }
  ret = KeyValueDB::test_init(superblock.omap_backend, omap_dir);
  if (ret < 0) {
    derr << "mkfs failed to create " << g_conf->filestore_omap_backend << dendl;
    ret = -1;
    goto close_fsid_fd;
  }
  dout(1) << g_conf->filestore_omap_backend << " db exists/created" << dendl;

  // journal?
  ret = mkjournal();
  if (ret)
    goto close_fsid_fd;

  dout(1) << "mkfs done in " << basedir << dendl;
  ret = 0;

 close_fsid_fd:
  VOID_TEMP_FAILURE_RETRY(::close(fsid_fd));
  fsid_fd = -1;
 close_basedir_fd:
  VOID_TEMP_FAILURE_RETRY(::close(basedir_fd));
  delete backend;
  backend = NULL;
  return ret;
}

int XStore::mkjournal()
{
  // read fsid
  int ret;
  char fn[PATH_MAX];
  snprintf(fn, sizeof(fn), "%s/fsid", basedir.c_str());
  int fd = ::open(fn, O_RDONLY, 0644);
  if (fd < 0) {
    int err = errno;
    derr << "XStore::mkjournal: open error: " << cpp_strerror(err) << dendl;
    return -err;
  }
  ret = read_fsid(fd, &fsid);
  if (ret < 0) {
    derr << "XStore::mkjournal: read error: " << cpp_strerror(ret) << dendl;
    VOID_TEMP_FAILURE_RETRY(::close(fd));
    return ret;
  }
  VOID_TEMP_FAILURE_RETRY(::close(fd));

  ret = 0;

  open_journal();
  if (journal) {
    ret = journal->check();
    if (ret < 0) {
      ret = journal->create();
      if (ret)
	derr << "mkjournal error creating journal on " << journalpath
		<< ": " << cpp_strerror(ret) << dendl;
      else
	dout(0) << "mkjournal created journal on " << journalpath << dendl;
    }
    delete journal;
    journal = 0;
  }
  return ret;
}

int XStore::read_fsid(int fd, uuid_d *uuid)
{
  char fsid_str[40];
  int ret = safe_read(fd, fsid_str, sizeof(fsid_str));
  if (ret < 0)
    return ret;
  if (ret == 8) {
    // old 64-bit fsid... mirror it.
    *(uint64_t*)&uuid->uuid[0] = *(uint64_t*)fsid_str;
    *(uint64_t*)&uuid->uuid[8] = *(uint64_t*)fsid_str;
    return 0;
  }

  if (ret > 36)
    fsid_str[36] = 0;
  if (!uuid->parse(fsid_str))
    return -EINVAL;
  return 0;
}

int XStore::lock_fsid()
{
  struct flock l;
  memset(&l, 0, sizeof(l));
  l.l_type = F_WRLCK;
  l.l_whence = SEEK_SET;
  l.l_start = 0;
  l.l_len = 0;
  int r = ::fcntl(fsid_fd, F_SETLK, &l);
  if (r < 0) {
    int err = errno;
    dout(0) << "lock_fsid failed to lock " << basedir << "/fsid, is another ceph-osd still running? "
	    << cpp_strerror(err) << dendl;
    return -err;
  }
  return 0;
}

bool XStore::test_mount_in_use()
{
  dout(5) << "test_mount basedir " << basedir << " journal " << journalpath << dendl;
  char fn[PATH_MAX];
  snprintf(fn, sizeof(fn), "%s/fsid", basedir.c_str());

  // verify fs isn't in use

  fsid_fd = ::open(fn, O_RDWR, 0644);
  if (fsid_fd < 0)
    return 0;   // no fsid, ok.
  bool inuse = lock_fsid() < 0;
  VOID_TEMP_FAILURE_RETRY(::close(fsid_fd));
  fsid_fd = -1;
  return inuse;
}

int XStore::_detect_fs()
{
  struct statfs st;
  int r = ::fstatfs(basedir_fd, &st);
  if (r < 0)
    return -errno;

  blk_size = st.f_bsize;

  create_backend(st.f_type);

  r = backend->detect_features();
  if (r < 0) {
    derr << "_detect_fs: detect_features error: " << cpp_strerror(r) << dendl;
    return r;
  }

  // test xattrs
  char fn[PATH_MAX];
  int x = rand();
  int y = x+1;
  snprintf(fn, sizeof(fn), "%s/xattr_test", basedir.c_str());
  int tmpfd = ::open(fn, O_CREAT|O_WRONLY|O_TRUNC, 0700);
  if (tmpfd < 0) {
    int ret = -errno;
    derr << "_detect_fs unable to create " << fn << ": " << cpp_strerror(ret) << dendl;
    return ret;
  }

  int ret = chain_fsetxattr(tmpfd, "user.test", &x, sizeof(x));
  if (ret >= 0)
    ret = chain_fgetxattr(tmpfd, "user.test", &y, sizeof(y));
  if ((ret < 0) || (x != y)) {
    derr << "Extended attributes don't appear to work. ";
    if (ret)
      *_dout << "Got error " + cpp_strerror(ret) + ". ";
    *_dout << "If you are using ext3 or ext4, be sure to mount the underlying "
	   << "file system with the 'user_xattr' option." << dendl;
    ::unlink(fn);
    VOID_TEMP_FAILURE_RETRY(::close(tmpfd));
    return -ENOTSUP;
  }

  char buf[1000];
  memset(buf, 0, sizeof(buf)); // shut up valgrind
  chain_fsetxattr(tmpfd, "user.test", &buf, sizeof(buf));
  chain_fsetxattr(tmpfd, "user.test2", &buf, sizeof(buf));
  chain_fsetxattr(tmpfd, "user.test3", &buf, sizeof(buf));
  chain_fsetxattr(tmpfd, "user.test4", &buf, sizeof(buf));
  ret = chain_fsetxattr(tmpfd, "user.test5", &buf, sizeof(buf));
  if (ret == -ENOSPC) {
    dout(0) << "limited size xattrs" << dendl;
  }
  chain_fremovexattr(tmpfd, "user.test");
  chain_fremovexattr(tmpfd, "user.test2");
  chain_fremovexattr(tmpfd, "user.test3");
  chain_fremovexattr(tmpfd, "user.test4");
  chain_fremovexattr(tmpfd, "user.test5");

  ::unlink(fn);
  VOID_TEMP_FAILURE_RETRY(::close(tmpfd));

  return 0;
}

int XStore::write_superblock()
{
  bufferlist bl;
  ::encode(superblock, bl);
  return safe_write_file(basedir.c_str(), "superblock",
      bl.c_str(), bl.length());
}

int XStore::read_superblock()
{
  bufferptr bp(PATH_MAX);
  int ret = safe_read_file(basedir.c_str(), "superblock",
      bp.c_str(), bp.length());
  if (ret < 0) {
    if (ret == -ENOENT) {
      // If the file doesn't exist write initial CompatSet
      return write_superblock();
    }
    return ret;
  }

  bufferlist bl;
  bl.push_back(bp);
  bufferlist::iterator i = bl.begin();
  ::decode(superblock, i);
  return 0;
}

void XStore::set_allow_sharded_objects()
{
  if (!get_allow_sharded_objects()) {
    superblock.compat_features.incompat.insert(CEPH_FS_FEATURE_INCOMPAT_SHARDS);
    int ret = write_superblock();
    assert(ret == 0);	//Should we return error and make caller handle it?
  }
  return;
}

bool XStore::get_allow_sharded_objects()
{
  return g_conf->filestore_debug_disable_sharded_check ||
    superblock.compat_features.incompat.contains(CEPH_FS_FEATURE_INCOMPAT_SHARDS);
}

int XStore::update_version_stamp()
{
  return write_version_stamp();
}

int XStore::version_stamp_is_valid(uint32_t *version)
{
  bufferptr bp(PATH_MAX);
  int ret = safe_read_file(basedir.c_str(), "store_version",
      bp.c_str(), bp.length());
  if (ret < 0) {
    if (ret == -ENOENT)
      return 0;
    return ret;
  }
  bufferlist bl;
  bl.push_back(bp);
  bufferlist::iterator i = bl.begin();
  ::decode(*version, i);
  if (*version == target_version)
    return 1;
  else
    return 0;
}

int XStore::write_version_stamp()
{
  bufferlist bl;
  ::encode(target_version, bl);

  return safe_write_file(basedir.c_str(), "store_version",
      bl.c_str(), bl.length());
}

int XStore::upgrade()
{
  uint32_t version;
  int r = version_stamp_is_valid(&version);
  if (r < 0)
    return r;
  if (r == 1)
    return 0;

  if (version < 3) {
    derr << "ObjectStore is old at version " << version << ".  Please upgrade to firefly v0.80.x, convert your store, and then upgrade."  << dendl;
    return -EINVAL;
  }

  // nothing necessary in XStore for v3 -> v4 upgrade; we just need to
  // open up DBObjectMap with the do_upgrade flag, which we already did.
  update_version_stamp();
  return 0;
}

int XStore::read_op_seq(uint64_t *seq)
{
  int op_fd = ::open(current_op_seq_fn.c_str(), O_CREAT|O_RDWR, 0644);
  if (op_fd < 0) {
    int r = -errno;
    assert(!m_filestore_fail_eio || r != -EIO);
    return r;
  }
  char s[40];
  memset(s, 0, sizeof(s));
  int ret = safe_read(op_fd, s, sizeof(s) - 1);
  if (ret < 0) {
    derr << "error reading " << current_op_seq_fn << ": " << cpp_strerror(ret) << dendl;
    VOID_TEMP_FAILURE_RETRY(::close(op_fd));
    assert(!m_filestore_fail_eio || ret != -EIO);
    return ret;
  }
  *seq = atoll(s);
  return op_fd;
}

int XStore::write_op_seq(int fd, uint64_t seq)
{
  char s[30];
  snprintf(s, sizeof(s), "%" PRId64 "\n", seq);
  int ret = TEMP_FAILURE_RETRY(::pwrite(fd, s, strlen(s), 0));
  if (ret < 0) {
    ret = -errno;
    assert(!m_filestore_fail_eio || ret != -EIO);
  }
  return ret;
}

int XStore::mount()
{
  int ret;
  char buf[PATH_MAX];
  uint64_t initial_op_seq;
  set<string> cluster_snaps;
  CompatSet supported_compat_set = get_fs_supported_compat_set();

  dout(5) << "basedir " << basedir << " journal " << journalpath << dendl;
  
  // make sure global base dir exists
  if (::access(basedir.c_str(), R_OK | W_OK)) {
    ret = -errno;
    derr << "XStore::mount: unable to access basedir '" << basedir << "': "
	 << cpp_strerror(ret) << dendl;
    goto done;
  }

  // get fsid
  snprintf(buf, sizeof(buf), "%s/fsid", basedir.c_str());
  fsid_fd = ::open(buf, O_RDWR, 0644);
  if (fsid_fd < 0) {
    ret = -errno;
    derr << "XStore::mount: error opening '" << buf << "': "
	 << cpp_strerror(ret) << dendl;
    goto done;
  }

  ret = read_fsid(fsid_fd, &fsid);
  if (ret < 0) {
    derr << "XStore::mount: error reading fsid_fd: " << cpp_strerror(ret)
	 << dendl;
    goto close_fsid_fd;
  }

  if (lock_fsid() < 0) {
    derr << "XStore::mount: lock_fsid failed" << dendl;
    ret = -EBUSY;
    goto close_fsid_fd;
  }

  dout(10) << "mount fsid is " << fsid << dendl;


  uint32_t version_stamp;
  ret = version_stamp_is_valid(&version_stamp);
  if (ret < 0) {
    derr << "XStore::mount : error in version_stamp_is_valid: "
	 << cpp_strerror(ret) << dendl;
    goto close_fsid_fd;
  } else if (ret == 0) {
    if (do_update || (int)version_stamp < g_conf->filestore_update_to) {
      derr << "XStore::mount : stale version stamp detected: "
	   << version_stamp 
	   << ". Proceeding, do_update "
	   << "is set, performing disk format upgrade."
	   << dendl;
      do_update = true;
    } else {
      ret = -EINVAL;
      derr << "XStore::mount : stale version stamp " << version_stamp
	   << ". Please run the XStore update script before starting the "
	   << "OSD, or set filestore_update_to to " << target_version
	   << " (currently " << g_conf->filestore_update_to << ")"
	   << dendl;
      goto close_fsid_fd;
    }
  }

  ret = read_superblock();
  if (ret < 0) {
    ret = -EINVAL;
    goto close_fsid_fd;
  }

  // Check if this XStore supports all the necessary features to mount
  if (supported_compat_set.compare(superblock.compat_features) == -1) {
    derr << "XStore::mount : Incompatible features set "
	   << superblock.compat_features << dendl;
    ret = -EINVAL;
    goto close_fsid_fd;
  }

  // open some dir handles
  basedir_fd = ::open(basedir.c_str(), O_RDONLY);
  if (basedir_fd < 0) {
    ret = -errno;
    derr << "XStore::mount: failed to open " << basedir << ": "
	 << cpp_strerror(ret) << dendl;
    basedir_fd = -1;
    goto close_fsid_fd;
  }

  // test for btrfs, xattrs, etc.
  ret = _detect_fs();
  if (ret < 0) {
    derr << "XStore::mount : error in _detect_fs: "
	 << cpp_strerror(ret) << dendl;
    goto close_basedir_fd;
  }

  {
    list<string> ls;
    ret = backend->list_checkpoints(ls);
    if (ret < 0) {
      derr << "XStore::mount : error in _list_snaps: "<< cpp_strerror(ret) << dendl;
      goto close_basedir_fd;
    }

    long long unsigned c, prev = 0;
    char clustersnap[NAME_MAX];
    for (list<string>::iterator it = ls.begin(); it != ls.end(); ++it) {
      if (sscanf(it->c_str(), COMMIT_SNAP_ITEM, &c) == 1) {
	assert(c > prev);
	prev = c;
	snaps.push_back(c);
      } else if (sscanf(it->c_str(), CLUSTER_SNAP_ITEM, clustersnap) == 1)
	cluster_snaps.insert(*it);
    }
  }

  if (m_osd_rollback_to_cluster_snap.length() &&
      cluster_snaps.count(m_osd_rollback_to_cluster_snap) == 0) {
    derr << "rollback to cluster snapshot '" << m_osd_rollback_to_cluster_snap << "': not found" << dendl;
    ret = -ENOENT;
    goto close_basedir_fd;
  }

  char nosnapfn[200];
  snprintf(nosnapfn, sizeof(nosnapfn), "%s/nosnap", current_fn.c_str());

  if (backend->can_checkpoint()) {
    if (snaps.empty()) {
      dout(0) << "mount WARNING: no consistent snaps found, store may be in inconsistent state" << dendl;
    } else {
      char s[NAME_MAX];
      uint64_t curr_seq = 0;

      if (m_osd_rollback_to_cluster_snap.length()) {
	derr << TEXT_RED
	     << " ** NOTE: rolling back to cluster snapshot " << m_osd_rollback_to_cluster_snap << " **"
	     << TEXT_NORMAL
	     << dendl;
	assert(cluster_snaps.count(m_osd_rollback_to_cluster_snap));
	snprintf(s, sizeof(s), CLUSTER_SNAP_ITEM, m_osd_rollback_to_cluster_snap.c_str());
      } else {
	{
	  int fd = read_op_seq(&curr_seq);
	  if (fd >= 0) {
	    VOID_TEMP_FAILURE_RETRY(::close(fd));
	  }
	}
	if (curr_seq)
	  dout(10) << " current/ seq was " << curr_seq << dendl;
	else
	  dout(10) << " current/ missing entirely (unusual, but okay)" << dendl;

	uint64_t cp = snaps.back();
	dout(10) << " most recent snap from " << snaps << " is " << cp << dendl;

	// if current/ is marked as non-snapshotted, refuse to roll
	// back (without clear direction) to avoid throwing out new
	// data.
	struct stat st;
	if (::stat(nosnapfn, &st) == 0) {
	  if (!m_osd_use_stale_snap) {
	    derr << "ERROR: " << nosnapfn << " exists, not rolling back to avoid losing new data" << dendl;
	    derr << "Force rollback to old snapshotted version with 'osd use stale snap = true'" << dendl;
	    derr << "config option for --osd-use-stale-snap startup argument." << dendl;
	    ret = -ENOTSUP;
	    goto close_basedir_fd;
	  }
	  derr << "WARNING: user forced start with data sequence mismatch: current was " << curr_seq
	       << ", newest snap is " << cp << dendl;
	  cerr << TEXT_YELLOW
	       << " ** WARNING: forcing the use of stale snapshot data **"
	       << TEXT_NORMAL << std::endl;
	}

        dout(10) << "mount rolling back to consistent snap " << cp << dendl;
	snprintf(s, sizeof(s), COMMIT_SNAP_ITEM, (long long unsigned)cp);
      }

      // drop current?
      ret = backend->rollback_to(s);
      if (ret) {
	derr << "XStore::mount: error rolling back to " << s << ": "
	     << cpp_strerror(ret) << dendl;
	goto close_basedir_fd;
      }
    }
  }
  initial_op_seq = 0;

  current_fd = ::open(current_fn.c_str(), O_RDONLY);
  if (current_fd < 0) {
    ret = -errno;
    derr << "XStore::mount: error opening: " << current_fn << ": " << cpp_strerror(ret) << dendl;
    goto close_basedir_fd;
  }

  assert(current_fd >= 0);

  op_fd = read_op_seq(&initial_op_seq);
  if (op_fd < 0) {
    derr << "XStore::mount: read_op_seq failed" << dendl;
    goto close_current_fd;
  }

  dout(5) << "mount op_seq is " << initial_op_seq << dendl;
  if (initial_op_seq == 0) {
    derr << "mount initial op seq is 0; something is wrong" << dendl;
    ret = -EINVAL;
    goto close_current_fd;
  }

  if (!backend->can_checkpoint()) {
    // mark current/ as non-snapshotted so that we don't rollback away
    // from it.
    int r = ::creat(nosnapfn, 0644);
    if (r < 0) {
      derr << "XStore::mount: failed to create current/nosnap" << dendl;
      goto close_current_fd;
    }
    VOID_TEMP_FAILURE_RETRY(::close(r));
  } else {
    // clear nosnap marker, if present.
    ::unlink(nosnapfn);
  }

  if (!(generic_flags & SKIP_MOUNT_OMAP)) {
    KeyValueDB * omap_store = KeyValueDB::create(g_ceph_context,
						 superblock.omap_backend,
						 omap_dir);
    if (omap_store == NULL)
    {
      derr << "Error creating " << superblock.omap_backend << dendl;
      ret = -1;
      goto close_current_fd;
    }

    omap_store->init();

    stringstream err;
    if (omap_store->create_and_open(err)) {
      delete omap_store;
      derr << "Error initializing " << superblock.omap_backend
	   << " : " << err.str() << dendl;
      ret = -1;
      goto close_current_fd;
    }

    DBObjectMap *dbomap = new DBObjectMap(omap_store);
    ret = dbomap->init(do_update);
    if (ret < 0) {
      delete dbomap;
      derr << "Error initializing DBObjectMap: " << ret << dendl;
      goto close_current_fd;
    }
    stringstream err2;

    if (g_conf->filestore_debug_omap_check && !dbomap->check(err2)) {
      derr << err2.str() << dendl;
      delete dbomap;
      ret = -EINVAL;
      goto close_current_fd;
    }
    object_map.reset(dbomap);
  }

  // journal
  open_journal();

  // select journal mode?
  if (journal) {
    journal->set_wait_on_full(true);
  } else {
    derr << "mount: no journal" << dendl;
    goto close_current_fd;
  }

  // Cleanup possibly invalid collections
  {
    vector<coll_t> collections;
    ret = list_collections(collections);
    if (ret < 0) {
      derr << "Error " << ret << " while listing collections" << dendl;
      goto close_current_fd;
    }
    for (vector<coll_t>::iterator i = collections.begin();
	 i != collections.end();
	 ++i) {
      Index index;
      ret = get_index(*i, &index);
      if (ret < 0) {
	derr << "Unable to mount index " << *i 
	     << " with error: " << ret << dendl;
	goto close_current_fd;
      }
      assert(NULL != index.index);
      RWLock::WLocker l((index.index)->access_lock);

      index->cleanup();
    }
  }

  for (int i = 0; i < wbthrottle_num; ++i) {
    wbthrottles[i]->start();
  }
  sync_thread.create();
  jwa_thread.create();

  if (!(generic_flags & SKIP_JOURNAL_REPLAY)) {
    ret = journal_replay(initial_op_seq);
    if (ret < 0) {
      derr << "mount failed to open journal " << journalpath << ": " << cpp_strerror(ret) << dendl;
      if (ret == -ENOTTY) {
        derr << "maybe journal is not pointing to a block device and its size "
	     << "wasn't configured?" << dendl;
      }

      // stop sync thread
      lock.Lock();
      stop = true;
      jwa_stop = true;
      sync_cond.Signal();
      lock.Unlock();
      sync_thread.join();
      jwa_thread.join();

      for (int i = 0; i < wbthrottle_num; ++i) {
        wbthrottles[i]->stop();
      }

      goto close_current_fd;
    }
  }

  {
    stringstream err2;
    if (g_conf->filestore_debug_omap_check && !object_map->check(err2)) {
      derr << err2.str() << dendl;
      ret = -EINVAL;
      goto close_current_fd;
    }
  }

  journal_start();

  op_tp.start();
  for (vector<Finisher*>::iterator it = ondisk_finishers.begin(); it != ondisk_finishers.end(); ++it) {
    (*it)->start();
  }
  for (vector<Finisher*>::iterator it = apply_finishers.begin(); it != apply_finishers.end(); ++it) {
    (*it)->start();
  }

  timer.init();

  // upgrade?
  if (g_conf->filestore_update_to >= (int)get_target_version()) {
    int err = upgrade();
    if (err < 0) {
      derr << "error converting store" << dendl;
      umount();
      return err;
    }
  }

  // all okay.
  return 0;

close_current_fd:
  VOID_TEMP_FAILURE_RETRY(::close(current_fd));
  current_fd = -1;
close_basedir_fd:
  VOID_TEMP_FAILURE_RETRY(::close(basedir_fd));
  basedir_fd = -1;
close_fsid_fd:
  VOID_TEMP_FAILURE_RETRY(::close(fsid_fd));
  fsid_fd = -1;
done:
  assert(!m_filestore_fail_eio || ret != -EIO);
  return ret;
}

int XStore::umount() 
{
  dout(5) << "umount " << basedir << dendl;
  
  do_force_sync();

  lock.Lock();
  stop = true;
  jwa_stop = true;
  sync_cond.Signal();
  lock.Unlock();
  sync_thread.join();
  jwa_thread.join();
  for (int i = 0; i < wbthrottle_num; ++i) {
    wbthrottles[i]->stop();
  }
  op_tp.stop();

  journal_stop();
  if (!(generic_flags & SKIP_JOURNAL_REPLAY))
    journal_write_close();

  for (vector<Finisher*>::iterator it = ondisk_finishers.begin(); it != ondisk_finishers.end(); ++it) {
    (*it)->stop();
  }
  for (vector<Finisher*>::iterator it = apply_finishers.begin(); it != apply_finishers.end(); ++it) {
    (*it)->stop();
  }

  if (fsid_fd >= 0) {
    VOID_TEMP_FAILURE_RETRY(::close(fsid_fd));
    fsid_fd = -1;
  }
  if (op_fd >= 0) {
    VOID_TEMP_FAILURE_RETRY(::close(op_fd));
    op_fd = -1;
  }
  if (current_fd >= 0) {
    VOID_TEMP_FAILURE_RETRY(::close(current_fd));
    current_fd = -1;
  }
  if (basedir_fd >= 0) {
    VOID_TEMP_FAILURE_RETRY(::close(basedir_fd));
    basedir_fd = -1;
  }

  force_sync = false;

  delete backend;
  backend = NULL;

  object_map.reset();

  {
    Mutex::Locker l(sync_entry_timeo_lock);
    timer.shutdown();
  }

  // nothing
  return 0;
}




/// -----------------------------

XStore::Op *XStore::build_op(list<Transaction*>& tls,
				   Context *ondisk,
				   Context *onreadable,
				   Context *onreadable_sync,
				   TrackedOpRef osd_op,
				   OpSequencer *osr)
{
  uint64_t bytes = 0, ops = 0;
  for (list<Transaction*>::iterator p = tls.begin();
       p != tls.end();
       ++p) {
    bytes += (*p)->get_num_bytes();
    ops += (*p)->get_num_ops();
  }

  Op *o = new Op;
  o->start = ceph_clock_now(g_ceph_context);
  o->tls.swap(tls);
  o->ondisk = ondisk;
  o->onreadable = onreadable;
  o->onreadable_sync = onreadable_sync;
  o->ops = ops;
  o->bytes = bytes;
  o->osd_op = osd_op;
  o->osr = osr;
  return o;
}



void XStore::queue_op(OpSequencer *osr, Op *o)
{
  // queue op on sequencer, then queue sequencer for the threadpool,
  // so that regardless of which order the threads pick up the
  // sequencer, the op order will be preserved.

  osr->queue(o);

  logger->inc(l_os_ops);
  logger->inc(l_os_bytes, o->bytes);

  dout(5) << "queue_op " << o << " seq " << o->op
	  << " " << *osr
	  << " " << o->bytes << " bytes"
	  << "   (queue has " << op_queue_len << " ops and " << op_queue_bytes << " bytes)"
	  << dendl;
  op_wq.queue(osr);
}

void XStore::op_queue_reserve_throttle(Op *o, ThreadPool::TPHandle *handle)
{
  // Do not call while holding the journal lock!
  uint64_t max_ops = m_filestore_queue_max_ops;
  uint64_t max_bytes = m_filestore_queue_max_bytes;

  if (backend->can_checkpoint() && is_committing()) {
    max_ops += m_filestore_queue_committing_max_ops;
    max_bytes += m_filestore_queue_committing_max_bytes;
  }

  logger->set(l_os_oq_max_ops, max_ops);
  logger->set(l_os_oq_max_bytes, max_bytes);

  utime_t start = ceph_clock_now(g_ceph_context);
  {
    Mutex::Locker l(op_throttle_lock);
    while ((max_ops && (op_queue_len + 1) > max_ops) ||
           (max_bytes && op_queue_bytes      // let single large ops through!
	      && (op_queue_bytes + o->bytes) > max_bytes)) {
      dout(2) << "waiting " << op_queue_len + 1 << " > " << max_ops << " ops || "
	      << op_queue_bytes + o->bytes << " > " << max_bytes << dendl;
      if (handle)
	handle->suspend_tp_timeout();
      op_throttle_cond.Wait(op_throttle_lock);
      if (handle)
	handle->reset_tp_timeout();
    }

    op_queue_len++;
    op_queue_bytes += o->bytes;
  }
  utime_t end = ceph_clock_now(g_ceph_context);
  logger->tinc(l_os_queue_lat, end - start);

  logger->set(l_os_oq_ops, op_queue_len);
  logger->set(l_os_oq_bytes, op_queue_bytes);
}

void XStore::op_queue_release_throttle(Op *o)
{
  {
    Mutex::Locker l(op_throttle_lock);
    op_queue_len--;
    op_queue_bytes -= o->bytes;
    op_throttle_cond.Signal();
  }

  logger->set(l_os_oq_ops, op_queue_len);
  logger->set(l_os_oq_bytes, op_queue_bytes);
}

void XStore::_do_op(OpSequencer *osr, ThreadPool::TPHandle &handle)
{  
  wbthrottles[osr->id % wbthrottle_num]->throttle();
  // inject a stall?
  if (g_conf->filestore_inject_stall) {
    int orig = g_conf->filestore_inject_stall;
    dout(5) << "_do_op filestore_inject_stall " << orig << ", sleeping" << dendl;
    for (int n = 0; n < g_conf->filestore_inject_stall; n++)
      sleep(1);
    g_conf->set_val("filestore_inject_stall", "0");
    dout(5) << "_do_op done stalling" << dendl;
  }

  osr->apply_lock.Lock();
  Op *o = osr->peek_queue();
  assert(o->state == Op::STATE_ACK || o->state == Op::STATE_INIT);
  if (o->state == Op::STATE_INIT) {
    apply_manager.op_apply_start(o->op);
  }
  dout(5) << "_do_op " << o << " seq " << o->op << " " << *osr << "/" << osr->parent << " start" << dendl;
  int r = 0;
  if (o->state == Op::STATE_ACK || !(o->wal)) {
    r = _do_transactions(o->tls, o->op, o, &handle);
  } else {
    dout(10) << "_do_op skip " << o << " seq " << o->op << " r = " << r
	     << ", finisher " << o->onreadable << " " << o->onreadable_sync << dendl;
  }
  if (o->state == Op::STATE_ACK) {
    apply_manager.op_apply_finish(o->op);
  }
  dout(10) << "_do_op " << o << " seq " << o->op << " r = " << r
	   << ", finisher " << o->onreadable << " " << o->onreadable_sync << dendl;
}

void XStore::_finish_op(OpSequencer *osr)
{
  Op *o = osr->peek_queue();
  if (o->state != Op::STATE_ACK) {
    Mutex::Locker l(jwa_lock);
    if (o->state == Op::STATE_INIT) {
      o->state = Op::STATE_WRITE;
    } else if (o->state == Op::STATE_JOURNAL) {
      o->state = Op::STATE_COMMIT;
      jwa_queue.push_back(o);
      jwa_cond.SignalOne();
    }
    osr->dequeue();
    osr->apply_lock.Unlock();  // locked in _do_op
    return;
  }

  o->state = Op::STATE_DONE;

  list<Context*> to_queue;
  o = osr->dequeue(&to_queue);
  assert(osr->dequeue_inq() == o);
  
  dout(10) << "_finish_op " << o << " seq " << o->op << " " << *osr << "/" << osr->parent << dendl;
  osr->apply_lock.Unlock();  // locked in _do_op

  // called with tp lock held
  op_queue_release_throttle(o);

  utime_t lat = ceph_clock_now(g_ceph_context);
  lat -= o->start;
  logger->tinc(l_os_apply_lat, lat);

  if (o->onreadable_sync) {
    o->onreadable_sync->complete(0);
  }
  if (o->onreadable) {
    apply_finishers[osr->id % apply_finisher_num]->queue(o->onreadable);
  }
  if (!to_queue.empty()) {
    apply_finishers[osr->id % apply_finisher_num]->queue(to_queue);
  }
  delete o;
}


struct C_JournaledWritten : public Context {
  XStore *fs;
  XStore::Op *o;

  C_JournaledWritten(XStore *f, XStore::Op *o): fs(f), o(o) { }
  void finish(int r) {
    fs->_journaled_written(o);
  }
};

struct C_JournaledAckWritten : public Context {
  XStore *fs;
  list<XStore::Op *> acks;

  C_JournaledAckWritten(XStore *f, list<XStore::Op *> acks): fs(f), acks(acks) {}
  void finish(int r) {
    fs->_journaled_ack_written(acks);
  }
};

void XStore::_jwa_entry()
{
  dout(10) << __func__ << " start" << dendl;
  list<Transaction*> tls;
  jwa_lock.Lock();
  while (true) {
    if (jwa_queue.empty()) {
      if (jwa_stop)
        break;
      dout(20) << __func__ << " sleep" << dendl;
      jwa_cond.Wait(jwa_lock); 
      dout(20) << __func__ << " wake" << dendl;
    } else {
      list<Op*> jwa_q;
      jwa_q.swap(jwa_queue);
      jwa_lock.Unlock();

      bufferlist bl;
      ::encode(jwa_q, bl);
      Op *o = build_op(tls, new C_JournaledAckWritten(this, jwa_q),
                       NULL, NULL, TrackedOpRef(), NULL);
      bufferlist tbl;
      int orig_len = journal->prepare_ack_entry(bl, tbl);
      uint64_t op_num = submit_manager.op_submit_start();
      o->op = op_num;
      o->osr = NULL;
      
      // FIXME osr->queue_journal(o->op);
      if (journal && journal->is_writeable()) {
        journal->submit_entry(op_num, tbl, orig_len, NULL, TrackedOpRef());
        submit_manager.op_submit_finish(op_num);
      } else {
        assert(0 == "Unexpected IO PATH");
      }
      jwa_lock.Lock();
    }
  }
  jwa_lock.Unlock();
  dout(10) << __func__ << " end" << dendl;
}

int XStore::get_replay_txns(list<Transaction*>& tls,
  list<Transaction*>* jtls, uint64_t seq, bool txns_done)
{
  bool should_wal = _should_wal(tls);
  if (should_wal && txns_done) {
    return 0;
  }

  Transaction* jtran = new Transaction();
  for (list<Transaction*>::iterator p = tls.begin();
      p != tls.end(); ++p) {
    Transaction::iterator i = (*p)->begin();
    while (i.have_op()) {
      Transaction::Op *op = i.decode_op();

      switch (op->op) {
      case Transaction::OP_NOP:
        break;
      case Transaction::OP_TOUCH:
        break;
        
      case Transaction::OP_WRITE:
        {
          const coll_t& cid = i.get_cid(op->cid);
          const ghobject_t& oid = i.get_oid(op->oid);
          uint64_t off = op->off;
          uint64_t len = op->len;
          if (!txns_done) {
            bufferlist bl;
            i.decode_bl(bl);
            jtran->touch(cid, oid);
          }
          dout(15) << "write " << cid << "/" << oid << " " << off << "~" << len << dendl;
        }
        break;
        
      case Transaction::OP_ZERO:
        break;
        
      case Transaction::OP_TRIMCACHE:
        break;
        
      case Transaction::OP_TRUNCATE:
        break;
        
      case Transaction::OP_REMOVE:
        break;
        
      case Transaction::OP_SETATTR:
        {
          const coll_t& cid = i.get_cid(op->cid);
          const ghobject_t& oid = i.get_oid(op->oid);
          string name = i.decode_string();
          bufferlist bl;
          i.decode_bl(bl);
          if (!txns_done && name == OI_ATTR) {
            // set unstable flag
            object_info_t oi(bl);
            oi.set_unstable();
            bufferlist bv;
            ::encode(oi, bv);
            dout(20) << "oid " << oid << " version " << oi.version << " seq " << seq << dendl;
            jtran->setattr(cid, oid, name, bv);
          } else {
            jtran->setattr(cid, oid, name, bl);
          }
        }
        break;
        
      case Transaction::OP_SETATTRS:
        {
          const coll_t& cid = i.get_cid(op->cid);
          const ghobject_t& oid = i.get_oid(op->oid);
          map<string, bufferptr> aset;
          i.decode_attrset(aset);
          if (!txns_done && aset.count(OI_ATTR)) {
            // set unstable flag
            bufferlist bv;
            bv.push_back(aset.find(OI_ATTR)->second);
            object_info_t oi(bv);
            oi.set_unstable();
            bv.clear();
            ::encode(oi, bv);
            dout(20) << "oid " << oid << " version " << oi.version << " seq " << seq << dendl;
            bv.c_str();
            aset.insert(make_pair(string(OI_ATTR), bv.buffers().front()));
          }
          jtran->setattrs(cid, oid, aset);
        }
        break;

      case Transaction::OP_RMATTR:
        {
          i.decode_string();
        }
        break;

      case Transaction::OP_RMATTRS:
        break;
        
      case Transaction::OP_CLONE:
        break;

      case Transaction::OP_CLONERANGE:
        break;

      case Transaction::OP_CLONERANGE2:
        break;

      case Transaction::OP_MKCOLL:
        break;

      case Transaction::OP_COLL_HINT:
        {
          bufferlist hint;
          i.decode_bl(hint);
        }
        break;

      case Transaction::OP_RMCOLL:
        break;

      case Transaction::OP_COLL_ADD:
        {
          coll_t ocid = i.get_cid(op->cid);
          coll_t ncid = i.get_cid(op->dest_cid);
          ghobject_t oid = i.get_oid(op->oid);

          // always followed by OP_COLL_REMOVE
          Transaction::Op *op2 = i.decode_op();
          coll_t ocid2 = i.get_cid(op2->cid);
          ghobject_t oid2 = i.get_oid(op2->oid);
          assert(op2->op == Transaction::OP_COLL_REMOVE);
          assert(ocid2 == ocid);
          assert(oid2 == oid);

        }
        break;

      case Transaction::OP_COLL_MOVE:
        break;

      case Transaction::OP_COLL_MOVE_RENAME:
        break;

      case Transaction::OP_COLL_SETATTR:
        {
          i.decode_string();
          bufferlist bl;
          i.decode_bl(bl);
        }
        break;

      case Transaction::OP_COLL_RMATTR:
        {
          i.decode_string();
        }
        break;

      case Transaction::OP_STARTSYNC:
        break;

      case Transaction::OP_COLL_RENAME:
        break;

      case Transaction::OP_OMAP_CLEAR:
        break;
      case Transaction::OP_OMAP_SETKEYS:
        {
          map<string, bufferlist> aset;
          i.decode_attrset(aset);
        }
        break;
      case Transaction::OP_PGMETA_WRITE:
        {
          if (txns_done) {
            const coll_t& cid = i.get_cid(op->cid);
            const ghobject_t& oid = i.get_oid(op->oid);
            map<string, bufferlist> aset;
            i.decode_attrset(aset);
            jtran->pgmeta_setkeys(cid, oid, aset);
          }
        }
        break;
      case Transaction::OP_OMAP_RMKEYS:
        {
          set<string> keys;
          i.decode_keyset(keys);
        }
        break;
      case Transaction::OP_OMAP_RMKEYRANGE:
        {
          i.decode_string();
          i.decode_string();
        }
        break;
      case Transaction::OP_OMAP_SETHEADER:
        {
          bufferlist bl;
          i.decode_bl(bl);
        }
        break;
      case Transaction::OP_SPLIT_COLLECTION:
        break;
      case Transaction::OP_SPLIT_COLLECTION2:
        break;

      case Transaction::OP_SETALLOCHINT:
        break;

      default:
        derr << "bad op " << op->op << dendl;
        assert(0);
      }
    }
    if (should_wal) {
      dout(20) << " transaction dump:\n";
      JSONFormatter f(true);
      f.open_object_section("transaction");
      (*p)->dump(&f);
      f.close_section();
      f.flush(*_dout);
      *_dout << dendl;
      dout(25) << __func__ << " enable wal" << dendl;
    }
  }
  assert(jtls->empty());
  if (!should_wal)
    jtls->push_back(jtran);
  return 0;
}


bool XStore::_should_wal(list<Transaction*> &tls)
{
  bufferlist ops_bl;
  uint32_t ops = 0;
  bool wal = false;
  for (list<ObjectStore::Transaction*>::const_iterator p = tls.begin();
       p != tls.end();
       ++p) {
    if ((*p)->get_use_tbl()) {
      wal = true;
      break;
    }
    ops_bl.append((*p)->op_bl);
    ops += (*p)->data.ops;
  }
  if (!wal) {
    uint32_t not_wal_ops[] = {Transaction::OP_WRITE, Transaction::OP_SETATTRS,
      Transaction::OP_OMAP_SETKEYS};
    char* op_buffer_p = ops_bl.get_contiguous(0, ops * sizeof(Transaction::Op));
    char* op_p = op_buffer_p;
 
    for (uint32_t i = 0, j = 0; i < ops; i++) {
      Transaction::Op* op = reinterpret_cast<Transaction::Op*>(op_p);
      op_p += sizeof(Transaction::Op);
      if (i == 2 && op->op == Transaction::OP_OMAP_RMKEYS)
        continue;
      if (op->op == Transaction::OP_WRITE_AHEAD ||
          op->op != not_wal_ops[j] ||
          i >= sizeof(not_wal_ops) / sizeof(uint32_t)) {
        wal = true;
        break;
      }
      j++;
    }
  }
  
  if (wal) {
    for (list<ObjectStore::Transaction*>::const_iterator p = tls.begin();
         p != tls.end();
         ++p) {
      dout(20) << " transaction dump:\n";
      JSONFormatter f(true);
      f.open_object_section("transaction");
      (*p)->dump(&f);
      f.close_section();
      f.flush(*_dout);
      *_dout << dendl;
      dout(20) << __func__ << " do wal" << dendl;
    }
  }
  return wal;
}

int XStore::queue_transactions(Sequencer *posr, list<Transaction*> &tls,
				  TrackedOpRef osd_op,
				  ThreadPool::TPHandle *handle)
{
  Context *onreadable;
  Context *ondisk;
  Context *onreadable_sync;
  ObjectStore::Transaction::collect_contexts(
    tls, &onreadable, &ondisk, &onreadable_sync);
  if (g_conf->filestore_blackhole) {
    dout(0) << "queue_transactions filestore_blackhole = TRUE, dropping transaction" << dendl;
    delete ondisk;
    delete onreadable;
    delete onreadable_sync;
    return 0;
  }

  // set up the sequencer
  OpSequencer *osr;
  if (!posr)
    posr = &default_osr;
  if (posr->p) {
    osr = static_cast<OpSequencer *>(posr->p);
    dout(5) << "queue_transactions existing " << *osr << "/" << osr->parent << dendl; //<< " w/ q " << osr->q << dendl;
  } else {
    osr = new OpSequencer(next_osr_id++);
    osr->parent = posr;
    posr->p = osr;
    dout(5) << "queue_transactions new " << *osr << "/" << osr->parent << dendl;
  }

  // used to include osr information in tracepoints during transaction apply
  for (list<ObjectStore::Transaction*>::iterator i = tls.begin(); i != tls.end(); ++i) {
    (*i)->set_osr(osr);
  }

  if (journal && journal->is_writeable()) {
    Op *o = build_op(tls, ondisk, onreadable, onreadable_sync, osd_op, osr);
    o->wal = _should_wal(o->tls);
    o->osr = osr;
    op_queue_reserve_throttle(o, handle);
    journal->throttle();
    //prepare and encode transactions data out of lock
    bufferlist tbl;
    int orig_len = journal->_op_journal_transactions_prepare(o->tls, tbl);
    uint64_t op_num = submit_manager.op_submit_start();
    o->op = op_num;

    if (m_filestore_do_dump)
      dump_transactions(o->tls, o->op, osr);

    osr->queue_inq(o);
    if (o->wal) {
      o->state = Op::STATE_INIT;
      osr->queue_journal(o->op);
      _op_journal_transactions(tbl, orig_len, o->op,
                               new C_JournaledWritten(this, o),
                               osd_op);
      queue_op(osr, o);
    } else {
      o->state = Op::STATE_INIT;
      osr->queue_journal(o->op);
      _op_journal_transactions(tbl, orig_len, o->op,
                               new C_JournaledWritten(this, o),
                               osd_op);
      queue_op(osr, o);
    }
    
    submit_manager.op_submit_finish(op_num);
    return 0;
  }

  assert(0 == "Unexpected IO PATH");
}

void XStore::_journaled_written(Op *o)
{
  dout(5) << "_journaled_written " << o << " seq " << o->op << " " << o->osr << " " << o->tls << dendl;
  {
    Mutex::Locker l(jwa_lock);
    if (o->wal || o->state == Op::STATE_WRITE) {
      o->state = Op::STATE_COMMIT;
      jwa_queue.push_back(o);
      jwa_cond.SignalOne();
    } else {
      o->state = Op::STATE_JOURNAL;
    }
  }
}

void XStore::_journaled_ack_written(list<Op *> acks)
{
  for (list<Op*>::iterator it = acks.begin(); it != acks.end(); ++it) {
    OpSequencer *osr = (*it)->osr;
    Op *o = *it;
    assert(o->state == Op::STATE_COMMIT);
    Context *ondisk = (*it)->ondisk; 
    dout(5) << __func__ << o << " seq " << o->op << " " << *osr << " " << o->tls << dendl;
    o->state = Op::STATE_ACK;

    // this should queue in order because the journal does it's completions in order.
    queue_op(osr, o);

    list<Context*> to_queue;
    osr->dequeue_journal(&to_queue);

    // do ondisk completions async, to prevent any onreadable_sync completions
    // getting blocked behind an ondisk completion.
    if (ondisk) {
      dout(10) << " queueing ondisk " << ondisk << dendl;
      ondisk_finishers[osr->id % ondisk_finisher_num]->queue(ondisk);
    }
    if (!to_queue.empty()) {
      ondisk_finishers[osr->id % ondisk_finisher_num]->queue(to_queue);
    }
  }
}

int XStore::_do_transactions(
  list<Transaction*> &tls,
  uint64_t op_seq,
  Op* o,
  ThreadPool::TPHandle *handle)
{
  int r = 0;
  int trans_num = 0;

  for (list<Transaction*>::iterator p = tls.begin();
       p != tls.end();
       ++p, trans_num++) {
    r = _do_transaction(**p, op_seq, trans_num, o, handle);
    if (o && o->state == Op::STATE_INIT)
      break;
    if (r < 0)
      break;
    if (handle)
      handle->reset_tp_timeout();
  }
  
  return r;
}

void XStore::_set_global_replay_guard(coll_t cid,
					 const SequencerPosition &spos)
{
  if (backend->can_checkpoint())
    return;

  // sync all previous operations on this sequencer
  int ret = sync_filesystem(basedir_fd);
  if (ret < 0) {
    derr << __func__ << " :sync_filesytem error " << cpp_strerror(ret) << dendl;
    assert(0 == "_set_global_replay_guard failed");
  }

  char fn[PATH_MAX];
  get_cdir(cid, fn, sizeof(fn));
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    int err = errno;
    derr << __func__ << ": " << cid << " error " << cpp_strerror(err) << dendl;
    assert(0 == "_set_global_replay_guard failed");
  }

  _inject_failure();

  // then record that we did it
  bufferlist v;
  ::encode(spos, v);
  int r = chain_fsetxattr(fd, GLOBAL_REPLAY_GUARD_XATTR, v.c_str(), v.length(), 1);
  if (r < 0) {
    derr << __func__ << ": fsetxattr " << GLOBAL_REPLAY_GUARD_XATTR
	 << " got " << cpp_strerror(r) << dendl;
    assert(0 == "fsetxattr failed");
  }

  // and make sure our xattr is durable.
  ::fsync(fd);

  _inject_failure();

  VOID_TEMP_FAILURE_RETRY(::close(fd));
  dout(10) << __func__ << ": " << spos << " done" << dendl;
}

int XStore::_check_global_replay_guard(const coll_t& cid,
					  const SequencerPosition& spos)
{
  if (!replaying || backend->can_checkpoint())
    return 1;

  char fn[PATH_MAX];
  get_cdir(cid, fn, sizeof(fn));
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    dout(10) << __func__ << ": " << cid << " dne" << dendl;
    return 1;  // if collection does not exist, there is no guard, and we can replay.
  }

  char buf[100];
  int r = chain_fgetxattr(fd, GLOBAL_REPLAY_GUARD_XATTR, buf, sizeof(buf));
  if (r < 0) {
    dout(20) << __func__ << " no xattr" << dendl;
    assert(!m_filestore_fail_eio || r != -EIO);
    VOID_TEMP_FAILURE_RETRY(::close(fd));
    return 1;  // no xattr
  }
  bufferlist bl;
  bl.append(buf, r);

  SequencerPosition opos;
  bufferlist::iterator p = bl.begin();
  ::decode(opos, p);

  VOID_TEMP_FAILURE_RETRY(::close(fd));
  return spos >= opos ? 1 : -1;
}


void XStore::_set_replay_guard(coll_t cid,
                                  const SequencerPosition &spos,
                                  bool in_progress=false)
{
  char fn[PATH_MAX];
  get_cdir(cid, fn, sizeof(fn));
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    int err = errno;
    derr << "_set_replay_guard " << cid << " error " << cpp_strerror(err) << dendl;
    assert(0 == "_set_replay_guard failed");
  }
  _set_replay_guard(fd, spos, 0, in_progress);
  ::close(fd);
} 


void XStore::_set_replay_guard(int fd,
				  const SequencerPosition& spos,
				  const ghobject_t *hoid,
				  bool in_progress)
{
  if (backend->can_checkpoint())
    return;

  dout(10) << "_set_replay_guard " << spos << (in_progress ? " START" : "") << dendl;

  _inject_failure();

  // first make sure the previous operation commits
  ::fsync(fd);

  // sync object_map too.  even if this object has a header or keys,
  // it have had them in the past and then removed them, so always
  // sync.
  if (hoid && hoid->is_pgmeta())
    pgmeta_cache.submit_pgmeta_keys(*hoid);
  object_map->sync(hoid, &spos);

  _inject_failure();

  // then record that we did it
  bufferlist v(40);
  ::encode(spos, v);
  ::encode(in_progress, v);
  int r = chain_fsetxattr(fd, REPLAY_GUARD_XATTR, v.c_str(), v.length(), 1);
  if (r < 0) {
    derr << "fsetxattr " << REPLAY_GUARD_XATTR << " got " << cpp_strerror(r) << dendl;
    assert(0 == "fsetxattr failed");
  }

  // and make sure our xattr is durable.
  ::fsync(fd);

  _inject_failure();

  dout(10) << "_set_replay_guard " << spos << " done" << dendl;
}

void XStore::_close_replay_guard(coll_t cid,
                                    const SequencerPosition &spos)
{
  char fn[PATH_MAX];
  get_cdir(cid, fn, sizeof(fn));
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    int err = errno;
    derr << "_close_replay_guard " << cid << " error " << cpp_strerror(err) << dendl;
    assert(0 == "_close_replay_guard failed");
  }
  _close_replay_guard(fd, spos);
  ::close(fd);
} 

void XStore::_close_replay_guard(int fd, const SequencerPosition& spos)
{
  if (backend->can_checkpoint())
    return;

  dout(10) << "_close_replay_guard " << spos << dendl;

  _inject_failure();

  // then record that we are done with this operation
  bufferlist v(40);
  ::encode(spos, v);
  bool in_progress = false;
  ::encode(in_progress, v);
  int r = chain_fsetxattr(fd, REPLAY_GUARD_XATTR, v.c_str(), v.length(), 1);
  if (r < 0) {
    derr << "fsetxattr " << REPLAY_GUARD_XATTR << " got " << cpp_strerror(r) << dendl;
    assert(0 == "fsetxattr failed");
  }

  // and make sure our xattr is durable.
  ::fsync(fd);

  _inject_failure();

  dout(10) << "_close_replay_guard " << spos << " done" << dendl;
}

int XStore::_check_replay_guard(const coll_t& cid, const ghobject_t& oid, const SequencerPosition& spos)
{
  if (!replaying || backend->can_checkpoint())
    return 1;

  int r = _check_global_replay_guard(cid, spos);
  if (r < 0)
    return r;

  FDRef fd;
  r = lfn_open(cid, oid, false, &fd);
  if (r < 0) {
    dout(10) << "_check_replay_guard " << cid << " " << oid << " dne" << dendl;
    return 1;  // if file does not exist, there is no guard, and we can replay.
  }
  int ret = _check_replay_guard(**fd, spos);
  lfn_close(fd);
  return ret;
}

int XStore::_check_replay_guard(const coll_t& cid, const SequencerPosition& spos)
{
  if (!replaying || backend->can_checkpoint())
    return 1;

  char fn[PATH_MAX];
  get_cdir(cid, fn, sizeof(fn));
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    dout(10) << "_check_replay_guard " << cid << " dne" << dendl;
    return 1;  // if collection does not exist, there is no guard, and we can replay.
  }
  int ret = _check_replay_guard(fd, spos);
  VOID_TEMP_FAILURE_RETRY(::close(fd));
  return ret;
}

int XStore::_check_replay_guard(int fd, const SequencerPosition& spos)
{
  if (!replaying || backend->can_checkpoint())
    return 1;

  char buf[100];
  int r = chain_fgetxattr(fd, REPLAY_GUARD_XATTR, buf, sizeof(buf));
  if (r < 0) {
    dout(20) << "_check_replay_guard no xattr" << dendl;
    assert(!m_filestore_fail_eio || r != -EIO);
    return 1;  // no xattr
  }
  bufferlist bl;
  bl.append(buf, r);

  SequencerPosition opos;
  bufferlist::iterator p = bl.begin();
  ::decode(opos, p);
  bool in_progress = false;
  if (!p.end())   // older journals don't have this
    ::decode(in_progress, p);
  if (opos > spos) {
    dout(10) << "_check_replay_guard object has " << opos << " > current pos " << spos
	     << ", now or in future, SKIPPING REPLAY" << dendl;
    return -1;
  } else if (opos == spos) {
    if (in_progress) {
      dout(10) << "_check_replay_guard object has " << opos << " == current pos " << spos
	       << ", in_progress=true, CONDITIONAL REPLAY" << dendl;
      return 0;
    } else {
      dout(10) << "_check_replay_guard object has " << opos << " == current pos " << spos
	       << ", in_progress=false, SKIPPING REPLAY" << dendl;
      return -1;
    }
  } else {
    dout(10) << "_check_replay_guard object has " << opos << " < current pos " << spos
	     << ", in past, will replay" << dendl;
    return 1;
  }
}

unsigned XStore::_do_transaction(
  Transaction& t, uint64_t op_seq, int trans_num, Op *o,
  ThreadPool::TPHandle *handle)
{

#ifdef WITH_LTTNG
  const char *osr_name = t.get_osr() ? static_cast<OpSequencer*>(t.get_osr())->get_name().c_str() : "<NULL>";
#endif
  int osr = t.get_osr() ? static_cast<OpSequencer*>(t.get_osr())->id : 0;

  dout(10) << "_do_transaction on " << &t << " osr " << osr << dendl;

  Transaction::iterator i = t.begin();
  
  SequencerPosition spos(op_seq, trans_num, 0);

  bool do_txn_pause = false;

  while (i.have_op() && !do_txn_pause) {
    if (handle)
      handle->reset_tp_timeout();

    Transaction::Op *op = i.decode_op();
    int r = 0;

    _inject_failure();

    switch (op->op) {
    case Transaction::OP_NOP:
      break;
    case Transaction::OP_TOUCH:
      {
        const coll_t& cid = i.get_cid(op->cid);
        const ghobject_t& oid = i.get_oid(op->oid);
        tracepoint(objectstore, touch_enter, osr_name);
        if (_check_replay_guard(cid, oid, spos) > 0)
          r = _touch(cid, oid);
        tracepoint(objectstore, touch_exit, r);
      }
      break;
      
    case Transaction::OP_WRITE:
      {
        const coll_t& cid = i.get_cid(op->cid);
        const ghobject_t& oid = i.get_oid(op->oid);
        uint64_t off = op->off;
        uint64_t len = op->len;
        uint32_t fadvise_flags = i.get_fadvise_flags();
        bufferlist bl;
        i.decode_bl(bl);
        tracepoint(objectstore, write_enter, osr_name, off, len);

        if (o && o->state == Op::STATE_INIT) {
          assert(trans_num == 0 && spos.op == 0);
        }

        if (_check_replay_guard(cid, oid, spos) > 0)
          if (!o || o->state == Op::STATE_INIT || o->wal) {
            r = _write(cid, oid, off, len, bl, fadvise_flags, osr);
            do_txn_pause = true;
          }
        tracepoint(objectstore, write_exit, r);
      }
      break;
      
    case Transaction::OP_ZERO:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        uint64_t off = op->off;
        uint64_t len = op->len;
        tracepoint(objectstore, zero_enter, osr_name, off, len);
        if (_check_replay_guard(cid, oid, spos) > 0)
          r = _zero(cid, oid, off, len, osr);
        tracepoint(objectstore, zero_exit, r);
      }
      break;
      
    case Transaction::OP_TRIMCACHE:
      {
	// deprecated, no-op
      }
      break;
      
    case Transaction::OP_TRUNCATE:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        uint64_t off = op->off;
        tracepoint(objectstore, truncate_enter, osr_name, off);
        if (_check_replay_guard(cid, oid, spos) > 0)
          r = _truncate(cid, oid, off);
        tracepoint(objectstore, truncate_exit, r);
      }
      break;
      
    case Transaction::OP_REMOVE:
      {
        const coll_t& cid = i.get_cid(op->cid);
        const ghobject_t& oid = i.get_oid(op->oid);
        tracepoint(objectstore, remove_enter, osr_name);
        if (_check_replay_guard(cid, oid, spos) > 0)
          r = _remove(cid, oid, spos, osr);
        tracepoint(objectstore, remove_exit, r);
      }
      break;
      
    case Transaction::OP_SETATTR:
      {
        const coll_t& cid = i.get_cid(op->cid);
        const ghobject_t& oid = i.get_oid(op->oid);
        string name = i.decode_string();
        bufferlist bl;
        i.decode_bl(bl);
        tracepoint(objectstore, setattr_enter, osr_name);
        if (_check_replay_guard(cid, oid, spos) > 0) {
          map<string, bufferptr> to_set;
          to_set[name] = bufferptr(bl.c_str(), bl.length());
          r = _setattrs(cid, oid, to_set, spos);
          if (r == -ENOSPC)
            dout(0) << " ENOSPC on setxattr on " << cid << "/" << oid
                    << " name " << name << " size " << bl.length() << dendl;
        }
        tracepoint(objectstore, setattr_exit, r);
      }
      break;
      
    case Transaction::OP_SETATTRS:
      {
        const coll_t& cid = i.get_cid(op->cid);
        const ghobject_t& oid = i.get_oid(op->oid);
        map<string, bufferptr> aset;
        i.decode_attrset(aset);
        tracepoint(objectstore, setattrs_enter, osr_name);
        if (_check_replay_guard(cid, oid, spos) > 0)
          r = _setattrs(cid, oid, aset, spos);
        tracepoint(objectstore, setattrs_exit, r);
        if (r == -ENOSPC)
          dout(0) << " ENOSPC on setxattrs on " << cid << "/" << oid << dendl;
      }
      break;

    case Transaction::OP_RMATTR:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        string name = i.decode_string();
        tracepoint(objectstore, rmattr_enter, osr_name);
        if (_check_replay_guard(cid, oid, spos) > 0)
          r = _rmattr(cid, oid, name.c_str(), spos);
        tracepoint(objectstore, rmattr_exit, r);
      }
      break;

    case Transaction::OP_RMATTRS:
      {
        const coll_t& cid = i.get_cid(op->cid);
        const ghobject_t& oid = i.get_oid(op->oid);
        tracepoint(objectstore, rmattrs_enter, osr_name);
        if (_check_replay_guard(cid, oid, spos) > 0)
          r = _rmattrs(cid, oid, spos);
        tracepoint(objectstore, rmattrs_exit, r);
      }
      break;
      
    case Transaction::OP_CLONE:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        ghobject_t noid = i.get_oid(op->dest_oid);
        tracepoint(objectstore, clone_enter, osr_name);
        r = _clone(cid, oid, noid, spos);
        tracepoint(objectstore, clone_exit, r);
      }
      break;

    case Transaction::OP_CLONERANGE:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        ghobject_t noid = i.get_oid(op->dest_oid);
        uint64_t off = op->off;
        uint64_t len = op->len;
        tracepoint(objectstore, clone_range_enter, osr_name, len);
        r = _clone_range(cid, oid, noid, off, len, off, spos);
        tracepoint(objectstore, clone_range_exit, r);
      }
      break;

    case Transaction::OP_CLONERANGE2:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        ghobject_t noid = i.get_oid(op->dest_oid);
        uint64_t srcoff = op->off;
        uint64_t len = op->len;
        uint64_t dstoff = op->dest_off;
        tracepoint(objectstore, clone_range2_enter, osr_name, len);
        r = _clone_range(cid, oid, noid, srcoff, len, dstoff, spos);
        tracepoint(objectstore, clone_range2_exit, r);
      }
      break;

    case Transaction::OP_MKCOLL:
      {
        coll_t cid = i.get_cid(op->cid);
        tracepoint(objectstore, mkcoll_enter, osr_name);
        if (_check_replay_guard(cid, spos) > 0)
          r = _create_collection(cid, spos);
        tracepoint(objectstore, mkcoll_exit, r);
      }
      break;

    case Transaction::OP_COLL_HINT:
      {
        coll_t cid = i.get_cid(op->cid);
        uint32_t type = op->hint_type;
        bufferlist hint;
        i.decode_bl(hint);
        bufferlist::iterator hiter = hint.begin();
        if (type == Transaction::COLL_HINT_EXPECTED_NUM_OBJECTS) {
          uint32_t pg_num;
          uint64_t num_objs;
          ::decode(pg_num, hiter);
          ::decode(num_objs, hiter);
          if (_check_replay_guard(cid, spos) > 0) {
            r = _collection_hint_expected_num_objs(cid, pg_num, num_objs, spos);
          }
        } else {
          // Ignore the hint
          dout(10) << "Unrecognized collection hint type: " << type << dendl;
        }
      }
      break;

    case Transaction::OP_RMCOLL:
      {
        coll_t cid = i.get_cid(op->cid);
        tracepoint(objectstore, rmcoll_enter, osr_name);
        if (_check_replay_guard(cid, spos) > 0)
          r = _destroy_collection(cid);
        tracepoint(objectstore, rmcoll_exit, r);
      }
      break;

    case Transaction::OP_COLL_ADD:
      {
        coll_t ocid = i.get_cid(op->cid);
        coll_t ncid = i.get_cid(op->dest_cid);
        ghobject_t oid = i.get_oid(op->oid);

        // always followed by OP_COLL_REMOVE
        Transaction::Op *op2 = i.decode_op();
        coll_t ocid2 = i.get_cid(op2->cid);
        ghobject_t oid2 = i.get_oid(op2->oid);
        assert(op2->op == Transaction::OP_COLL_REMOVE);
        assert(ocid2 == ocid);
        assert(oid2 == oid);

        tracepoint(objectstore, coll_add_enter);
        r = _collection_add(ncid, ocid, oid, spos);
        tracepoint(objectstore, coll_add_exit, r);
        spos.op++;
        if (r < 0)
          break;
        tracepoint(objectstore, coll_remove_enter, osr_name);
        if (_check_replay_guard(ocid, oid, spos) > 0)
          r = _remove(ocid, oid, spos, osr);
        tracepoint(objectstore, coll_remove_exit, r);
      }
      break;

    case Transaction::OP_COLL_MOVE:
      {
        // WARNING: this is deprecated and buggy; only here to replay old journals.
        coll_t ocid = i.get_cid(op->cid);
        coll_t ncid = i.get_cid(op->dest_cid);
        ghobject_t oid = i.get_oid(op->oid);
        tracepoint(objectstore, coll_move_enter);
        r = _collection_add(ocid, ncid, oid, spos);
        if (r == 0 &&
            (_check_replay_guard(ocid, oid, spos) > 0))
          r = _remove(ocid, oid, spos, osr);
        tracepoint(objectstore, coll_move_exit, r);
      }
      break;

    case Transaction::OP_COLL_MOVE_RENAME:
      {
        coll_t oldcid = i.get_cid(op->cid);
        ghobject_t oldoid = i.get_oid(op->oid);
        coll_t newcid = i.get_cid(op->dest_cid);
        ghobject_t newoid = i.get_oid(op->dest_oid);
        tracepoint(objectstore, coll_move_rename_enter);
        r = _collection_move_rename(oldcid, oldoid, newcid, newoid, spos, osr);
        tracepoint(objectstore, coll_move_rename_exit, r);
      }
      break;

    case Transaction::OP_COLL_SETATTR:
      {
        coll_t cid = i.get_cid(op->cid);
        string name = i.decode_string();
        bufferlist bl;
        i.decode_bl(bl);
        tracepoint(objectstore, coll_setattr_enter, osr_name);
        if (_check_replay_guard(cid, spos) > 0)
          r = _collection_setattr(cid, name.c_str(), bl.c_str(), bl.length());
        tracepoint(objectstore, coll_setattr_exit, r);
      }
      break;

    case Transaction::OP_COLL_RMATTR:
      {
        coll_t cid = i.get_cid(op->cid);
        string name = i.decode_string();
        tracepoint(objectstore, coll_rmattr_enter, osr_name);
        if (_check_replay_guard(cid, spos) > 0)
          r = _collection_rmattr(cid, name.c_str());
        tracepoint(objectstore, coll_rmattr_exit, r);
      }
      break;

    case Transaction::OP_STARTSYNC:
      tracepoint(objectstore, startsync_enter, osr_name);
      _start_sync();
      tracepoint(objectstore, startsync_exit);
      break;

    case Transaction::OP_COLL_RENAME:
      {
        r = -EOPNOTSUPP;
      }
      break;

    case Transaction::OP_OMAP_CLEAR:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        tracepoint(objectstore, omap_clear_enter, osr_name);
        r = _omap_clear(cid, oid, spos);
        tracepoint(objectstore, omap_clear_exit, r);
      }
      break;
    case Transaction::OP_OMAP_SETKEYS:
      {
        const coll_t& cid = i.get_cid(op->cid);
        const ghobject_t& oid = i.get_oid(op->oid);
        map<string, bufferlist> aset;
        i.decode_attrset(aset);
        tracepoint(objectstore, omap_setkeys_enter, osr_name);
        r = _omap_setkeys(cid, oid, aset, spos);
        tracepoint(objectstore, omap_setkeys_exit, r);
      }
      break;
    case Transaction::OP_OMAP_RMKEYS:
      {
        const coll_t& cid = i.get_cid(op->cid);
        const ghobject_t& oid = i.get_oid(op->oid);
        set<string> keys;
        i.decode_keyset(keys);
        tracepoint(objectstore, omap_rmkeys_enter, osr_name);
        r = _omap_rmkeys(cid, oid, keys, spos);
        tracepoint(objectstore, omap_rmkeys_exit, r);
      }
      break;
    case Transaction::OP_OMAP_RMKEYRANGE:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        string first, last;
        first = i.decode_string();
        last = i.decode_string();
        tracepoint(objectstore, omap_rmkeyrange_enter, osr_name);
        r = _omap_rmkeyrange(cid, oid, first, last, spos);
        tracepoint(objectstore, omap_rmkeyrange_exit, r);
      }
      break;
    case Transaction::OP_OMAP_SETHEADER:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        bufferlist bl;
        i.decode_bl(bl);
        tracepoint(objectstore, omap_setheader_enter, osr_name);
        r = _omap_setheader(cid, oid, bl, spos);
        tracepoint(objectstore, omap_setheader_exit, r);
      }
      break;
    case Transaction::OP_SPLIT_COLLECTION:
      {
        coll_t cid = i.get_cid(op->cid);
        uint32_t bits = op->split_bits;
        uint32_t rem = op->split_rem;
        coll_t dest = i.get_cid(op->dest_cid);
        tracepoint(objectstore, split_coll_enter, osr_name);
        r = _split_collection_create(cid, bits, rem, dest, spos);
        tracepoint(objectstore, split_coll_exit, r);
      }
      break;
    case Transaction::OP_SPLIT_COLLECTION2:
      {
        coll_t cid = i.get_cid(op->cid);
        uint32_t bits = op->split_bits;
        uint32_t rem = op->split_rem;
        coll_t dest = i.get_cid(op->dest_cid);
        tracepoint(objectstore, split_coll2_enter, osr_name);
        r = _split_collection(cid, bits, rem, dest, spos);
        tracepoint(objectstore, split_coll2_exit, r);
      }
      break;

    case Transaction::OP_SETALLOCHINT:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        uint64_t expected_object_size = op->expected_object_size;
        uint64_t expected_write_size = op->expected_write_size;
        tracepoint(objectstore, setallochint_enter, osr_name);
        if (_check_replay_guard(cid, oid, spos) > 0)
          r = _set_alloc_hint(cid, oid, expected_object_size,
                              expected_write_size);
        tracepoint(objectstore, setallochint_exit, r);
      }
      break;

    default:
      derr << "bad op " << op->op << dendl;
      assert(0);
    }

    if (r < 0) {
      bool ok = false;

      if (r == -ENOENT && !(op->op == Transaction::OP_CLONERANGE ||
			    op->op == Transaction::OP_CLONE ||
			    op->op == Transaction::OP_CLONERANGE2 ||
			    op->op == Transaction::OP_COLL_ADD))
	// -ENOENT is normally okay
	// ...including on a replayed OP_RMCOLL with checkpoint mode
	ok = true;
      if (r == -ENODATA)
	ok = true;

      if (op->op == Transaction::OP_SETALLOCHINT)
        // Either EOPNOTSUPP or EINVAL most probably.  EINVAL in most
        // cases means invalid hint size (e.g. too big, not a multiple
        // of block size, etc) or, at least on xfs, an attempt to set
        // or change it when the file is not empty.  However,
        // OP_SETALLOCHINT is advisory, so ignore all errors.
        ok = true;

      if (replaying && !backend->can_checkpoint()) {
	if (r == -EEXIST && op->op == Transaction::OP_MKCOLL) {
	  dout(10) << "tolerating EEXIST during journal replay since checkpoint is not enabled" << dendl;
	  ok = true;
	}
	if (r == -EEXIST && op->op == Transaction::OP_COLL_ADD) {
	  dout(10) << "tolerating EEXIST during journal replay since checkpoint is not enabled" << dendl;
	  ok = true;
	}
	if (r == -EEXIST && op->op == Transaction::OP_COLL_MOVE) {
	  dout(10) << "tolerating EEXIST during journal replay since checkpoint is not enabled" << dendl;
	  ok = true;
	}
	if (r == -ERANGE) {
	  dout(10) << "tolerating ERANGE on replay" << dendl;
	  ok = true;
	}
	if (r == -ENOENT) {
	  dout(10) << "tolerating ENOENT on replay" << dendl;
	  ok = true;
	}
      }

      if (!ok) {
	const char *msg = "unexpected error code";

	if (r == -ENOENT && (op->op == Transaction::OP_CLONERANGE ||
			     op->op == Transaction::OP_CLONE ||
			     op->op == Transaction::OP_CLONERANGE2))
	  msg = "ENOENT on clone suggests osd bug";

	if (r == -ENOSPC)
	  // For now, if we hit _any_ ENOSPC, crash, before we do any damage
	  // by partially applying transactions.
	  msg = "ENOSPC handling not implemented";

	if (r == -ENOTEMPTY) {
	  msg = "ENOTEMPTY suggests garbage data in osd data dir";
	}

	dout(0) << " error " << cpp_strerror(r) << " not handled on operation " << op
		<< " (" << spos << ", or op " << spos.op << ", counting from 0)" << dendl;
	dout(0) << msg << dendl;
	dout(0) << " transaction dump:\n";
	JSONFormatter f(true);
	f.open_object_section("transaction");
	t.dump(&f);
	f.close_section();
	f.flush(*_dout);
	*_dout << dendl;

	if (r == -EMFILE) {
	  dump_open_fds(g_ceph_context);
	}

	assert(0 == "unexpected error");
      }
    }

    spos.op++;
  }

  if (o && o->state == Op::STATE_INIT) {
    assert(do_txn_pause && trans_num == 0);
  }
  _inject_failure();

  return 0;  // FIXME count errors
}

  /*********************************************/



// --------------------
// objects

bool XStore::exists(coll_t cid, const ghobject_t& oid)
{
  tracepoint(objectstore, exists_enter, cid.c_str());
  struct stat st;
  bool retval = stat(cid, oid, &st) == 0;
  tracepoint(objectstore, exists_exit, retval);
  return retval;
}
  
int XStore::stat(
  coll_t cid, const ghobject_t& oid, struct stat *st, bool allow_eio)
{
  tracepoint(objectstore, stat_enter, cid.c_str());
  int r = lfn_stat(cid, oid, st);
  assert(allow_eio || !m_filestore_fail_eio || r != -EIO);
  if (r < 0) {
    dout(10) << "stat " << cid << "/" << oid
	     << " = " << r << dendl;
  } else {
    dout(10) << "stat " << cid << "/" << oid
	     << " = " << r
	     << " (size " << st->st_size << ")" << dendl;
  }
  if (g_conf->filestore_debug_inject_read_err &&
      debug_mdata_eio(oid)) {
    return -EIO;
  } else {
    tracepoint(objectstore, stat_exit, r);
    return r;
  }
}

int XStore::read(
  coll_t cid,
  const ghobject_t& oid,
  uint64_t offset,
  size_t len,
  bufferlist& bl,
  uint32_t op_flags,
  bool allow_eio)
{
  int got;
  tracepoint(objectstore, read_enter, cid.c_str(), offset, len);

  dout(15) << "read " << cid << "/" << oid << " " << offset << "~" << len << dendl;

  FDRef fd;
  int r = lfn_open(cid, oid, false, &fd);
  if (r < 0) {
    dout(10) << "XStore::read(" << cid << "/" << oid << ") open error: "
	     << cpp_strerror(r) << dendl;
    return r;
  }

  if (len == 0) {
    struct stat st;
    memset(&st, 0, sizeof(struct stat));
    int r = ::fstat(**fd, &st);
    assert(r == 0);
    len = st.st_size;
  }

#ifdef HAVE_POSIX_FADVISE
  if (op_flags & CEPH_OSD_OP_FLAG_FADVISE_RANDOM)
    posix_fadvise(**fd, offset, len, POSIX_FADV_RANDOM);
  if (op_flags & CEPH_OSD_OP_FLAG_FADVISE_SEQUENTIAL)
    posix_fadvise(**fd, offset, len, POSIX_FADV_SEQUENTIAL);
#endif

  bufferptr bptr(len);  // prealloc space for entire read
  got = safe_pread(**fd, bptr.c_str(), len, offset);
  if (got < 0) {
    dout(10) << "XStore::read(" << cid << "/" << oid << ") pread error: " << cpp_strerror(got) << dendl;
    lfn_close(fd);
    assert(allow_eio || !m_filestore_fail_eio || got != -EIO);
    return got;
  }
  bptr.set_length(got);   // properly size the buffer
  bl.push_back(bptr);   // put it in the target bufferlist

#ifdef HAVE_POSIX_FADVISE
  if (op_flags & CEPH_OSD_OP_FLAG_FADVISE_DONTNEED)
    posix_fadvise(**fd, offset, len, POSIX_FADV_DONTNEED);
  if (op_flags & (CEPH_OSD_OP_FLAG_FADVISE_RANDOM | CEPH_OSD_OP_FLAG_FADVISE_SEQUENTIAL))
    posix_fadvise(**fd, offset, len, POSIX_FADV_NORMAL);
#endif

  if (m_filestore_sloppy_crc && (!replaying || backend->can_checkpoint())) {
    ostringstream ss;
    int errors = backend->_crc_verify_read(**fd, offset, got, bl, &ss);
    if (errors > 0) {
      dout(0) << "XStore::read " << cid << "/" << oid << " " << offset << "~"
	      << got << " ... BAD CRC:\n" << ss.str() << dendl;
      assert(0 == "bad crc on read");
    }
  }

  lfn_close(fd);

  dout(10) << "XStore::read " << cid << "/" << oid << " " << offset << "~"
	   << got << "/" << len << dendl;
  if (g_conf->filestore_debug_inject_read_err &&
      debug_data_eio(oid)) {
    return -EIO;
  } else {
    tracepoint(objectstore, read_exit, got);
    return got;
  }
}

int XStore::fiemap(coll_t cid, const ghobject_t& oid,
                    uint64_t offset, size_t len,
                    bufferlist& bl)
{
  tracepoint(objectstore, fiemap_enter, cid.c_str(), offset, len);

  if (!backend->has_fiemap() || len <= (size_t)m_filestore_fiemap_threshold) {
    map<uint64_t, uint64_t> m;
    m[offset] = len;
    ::encode(m, bl);
    return 0;
  }


  struct fiemap *fiemap = NULL;
  map<uint64_t, uint64_t> exomap;

  dout(15) << "fiemap " << cid << "/" << oid << " " << offset << "~" << len << dendl;

  FDRef fd;
  int r = lfn_open(cid, oid, false, &fd);
  if (r < 0) {
    dout(10) << "read couldn't open " << cid << "/" << oid << ": " << cpp_strerror(r) << dendl;
  } else {
    uint64_t i;

    r = backend->do_fiemap(**fd, offset, len, &fiemap);
    if (r < 0)
      goto done;

    if (fiemap->fm_mapped_extents == 0) {
      free(fiemap);
      goto done;
    }

    struct fiemap_extent *extent = &fiemap->fm_extents[0];

    /* start where we were asked to start */
    if (extent->fe_logical < offset) {
      extent->fe_length -= offset - extent->fe_logical;
      extent->fe_logical = offset;
    }

    i = 0;

    while (i < fiemap->fm_mapped_extents) {
      struct fiemap_extent *next = extent + 1;

      dout(10) << "XStore::fiemap() fm_mapped_extents=" << fiemap->fm_mapped_extents
	       << " fe_logical=" << extent->fe_logical << " fe_length=" << extent->fe_length << dendl;

      /* try to merge extents */
      while ((i < fiemap->fm_mapped_extents - 1) &&
             (extent->fe_logical + extent->fe_length == next->fe_logical)) {
          next->fe_length += extent->fe_length;
          next->fe_logical = extent->fe_logical;
          extent = next;
          next = extent + 1;
          i++;
      }

      if (extent->fe_logical + extent->fe_length > offset + len)
        extent->fe_length = offset + len - extent->fe_logical;
      exomap[extent->fe_logical] = extent->fe_length;
      i++;
      extent++;
    }
    free(fiemap);
  }

done:
  if (r >= 0) {
    lfn_close(fd);
    ::encode(exomap, bl);
  }

  dout(10) << "fiemap " << cid << "/" << oid << " " << offset << "~" << len << " = " << r << " num_extents=" << exomap.size() << " " << exomap << dendl;
  assert(!m_filestore_fail_eio || r != -EIO);
  tracepoint(objectstore, fiemap_exit, r);
  return r;
}


int XStore::_remove(coll_t cid, const ghobject_t& oid,
		       const SequencerPosition &spos, int osr)
{
  dout(15) << "remove " << cid << "/" << oid << dendl;
  int r = lfn_unlink(cid, oid, spos, false, osr);
  dout(10) << "remove " << cid << "/" << oid << " = " << r << dendl;
  return r;
}

int XStore::_truncate(coll_t cid, const ghobject_t& oid, uint64_t size)
{
  dout(15) << "truncate " << cid << "/" << oid << " size " << size << dendl;
  int r = lfn_truncate(cid, oid, size);
  dout(10) << "truncate " << cid << "/" << oid << " size " << size << " = " << r << dendl;
  return r;
}


int XStore::_touch(coll_t cid, const ghobject_t& oid)
{
  dout(15) << "touch " << cid << "/" << oid << dendl;

  FDRef fd;
  int r = lfn_open(cid, oid, true, &fd);
  if (r < 0) {
    return r;
  } else {
    lfn_close(fd);
  }
  dout(10) << "touch " << cid << "/" << oid << " = " << r << dendl;
  return r;
}

int XStore::_write(coll_t cid, const ghobject_t& oid,
                     uint64_t offset, size_t len,
                     const bufferlist& bl, uint32_t fadvise_flags,
                     int osr)
{
  dout(15) << "write " << cid << "/" << oid << " " << offset << "~" << len << dendl;
  int r;

  int64_t actual;

  FDRef fd;
  r = lfn_open(cid, oid, true, &fd);
  if (r < 0) {
    dout(0) << "write couldn't open " << cid << "/"
	    << oid << ": "
	    << cpp_strerror(r) << dendl;
    goto out;
  }
    
  // seek
  actual = ::lseek64(**fd, offset, SEEK_SET);
  if (actual < 0) {
    r = -errno;
    dout(0) << "write lseek64 to " << offset << " failed: " << cpp_strerror(r) << dendl;
    lfn_close(fd);
    goto out;
  }
  if (actual != (int64_t)offset) {
    dout(0) << "write lseek64 to " << offset << " gave bad offset " << actual << dendl;
    r = -EIO;
    lfn_close(fd);
    goto out;
  }

  // write
  r = bl.write_fd(**fd);
  if (r == 0)
    r = bl.length();

  if (r >= 0 && m_filestore_sloppy_crc) {
    int rc = backend->_crc_update_write(**fd, offset, len, bl);
    assert(rc >= 0);
  }

  // flush?
  if (!replaying &&
      g_conf->filestore_wbthrottle_enable)
    wbthrottles[osr % wbthrottle_num]->queue_wb(fd, oid, offset, len,
			  fadvise_flags & CEPH_OSD_OP_FLAG_FADVISE_DONTNEED);
  lfn_close(fd);

 out:
  dout(10) << "write " << cid << "/" << oid << " " << offset << "~" << len << " = " << r << dendl;
  return r;
}

int XStore::_zero(coll_t cid, const ghobject_t& oid, uint64_t offset, size_t len, int osr)
{
  dout(15) << "zero " << cid << "/" << oid << " " << offset << "~" << len << dendl;
  int ret = 0;

#ifdef CEPH_HAVE_FALLOCATE
# if !defined(DARWIN) && !defined(__FreeBSD__)
  // first try to punch a hole.
  FDRef fd;
  ret = lfn_open(cid, oid, false, &fd);
  if (ret < 0) {
    goto out;
  }

  // first try fallocate
  ret = fallocate(**fd, FALLOC_FL_PUNCH_HOLE, offset, len);
  if (ret < 0)
    ret = -errno;
  lfn_close(fd);

  if (ret >= 0 && m_filestore_sloppy_crc) {
    int rc = backend->_crc_update_zero(**fd, offset, len);
    assert(rc >= 0);
  }

  if (ret == 0)
    goto out;  // yay!
  if (ret != -EOPNOTSUPP)
    goto out;  // some other error
# endif
#endif

  // lame, kernel is old and doesn't support it.
  // write zeros.. yuck!
  dout(20) << "zero FALLOC_FL_PUNCH_HOLE not supported, falling back to writing zeros" << dendl;
  {
    bufferptr bp(len);
    bp.zero();
    bufferlist bl;
    bl.push_back(bp);
    ret = _write(cid, oid, offset, len, bl, 0, osr);
  }

 out:
  dout(20) << "zero " << cid << "/" << oid << " " << offset << "~" << len << " = " << ret << dendl;
  return ret;
}

int XStore::_clone(coll_t cid, const ghobject_t& oldoid, const ghobject_t& newoid,
		      const SequencerPosition& spos)
{
  dout(15) << "clone " << cid << "/" << oldoid << " -> " << cid << "/" << newoid << dendl;

  if (_check_replay_guard(cid, newoid, spos) < 0)
    return 0;

  int r;
  FDRef o, n;
  {
    Index index;
    r = lfn_open(cid, oldoid, false, &o, &index);
    if (r < 0) {
      goto out2;
    }
    assert(NULL != (index.index));
    RWLock::WLocker l((index.index)->access_lock);

    r = lfn_open(cid, newoid, true, &n, &index);
    if (r < 0) {
      goto out;
    }
    r = ::ftruncate(**n, 0);
    if (r < 0) {
      goto out3;
    }
    struct stat st;
    ::fstat(**o, &st);
    r = _do_clone_range(**o, **n, 0, st.st_size, 0);
    if (r < 0) {
      r = -errno;
      goto out3;
    }

    dout(20) << "objectmap clone" << dendl;
    if (oldoid.is_pgmeta()) {
      r = pgmeta_cache.submit_pgmeta_keys(oldoid);
      if (r < 0)
        goto out3;
    }
    r = object_map->clone(oldoid, newoid, &spos);
    if (r < 0 && r != -ENOENT)
      goto out3;
  }

  {
    char buf[2];
    map<string, pair<bufferptr, int> > aset;
    r = _fgetattrs(**o, aset);
    if (r < 0)
      goto out3;

    r = chain_fgetxattr(**o, XATTR_SPILL_OUT_NAME, buf, sizeof(buf));
    if (r >= 0 && !strncmp(buf, XATTR_NO_SPILL_OUT, sizeof(XATTR_NO_SPILL_OUT))) {
      r = chain_fsetxattr(**n, XATTR_SPILL_OUT_NAME, XATTR_NO_SPILL_OUT,
                          sizeof(XATTR_NO_SPILL_OUT), 1);
    } else {
      r = chain_fsetxattr(**n, XATTR_SPILL_OUT_NAME, XATTR_SPILL_OUT,
                          sizeof(XATTR_SPILL_OUT), 1);
    }
    if (r < 0)
      goto out3;

    for (map<string, pair<bufferptr, int> >::iterator it = aset.begin();
         it != aset.end();
         ++it) {
      r = _fsetattr(**n, it->first, it->second.first, it->second.second);
      if (r < 0)
        goto out3;
      }
  }

  // clone is non-idempotent; record our work.
  _set_replay_guard(**n, spos, &newoid);

 out3:
  lfn_close(n);
 out:
  lfn_close(o);
 out2:
  dout(10) << "clone " << cid << "/" << oldoid << " -> " << cid << "/" << newoid << " = " << r << dendl;
  assert(!m_filestore_fail_eio || r != -EIO);
  return r;
}

int XStore::_do_clone_range(int from, int to, uint64_t srcoff, uint64_t len, uint64_t dstoff)
{
  dout(20) << "_do_clone_range copy " << srcoff << "~" << len << " to " << dstoff << dendl;
  return backend->clone_range(from, to, srcoff, len, dstoff);
}

int XStore::_do_sparse_copy_range(int from, int to, uint64_t srcoff, uint64_t len, uint64_t dstoff)
{
  dout(20) << __func__ << " " << srcoff << "~" << len << " to " << dstoff << dendl;
  int r = 0;
  struct fiemap *fiemap = NULL;

  // fiemap doesn't allow zero length
  if (len == 0)
    return 0;

  r = backend->do_fiemap(from, srcoff, len, &fiemap);
  if (r < 0) {
    derr << "do_fiemap failed:" << srcoff << "~" << len << " = " << r << dendl;
    return r;
  }

  // No need to copy
  if (fiemap->fm_mapped_extents == 0)
    return r;

  int buflen = 4096*32;
  char buf[buflen];
  struct fiemap_extent *extent = &fiemap->fm_extents[0];

  /* start where we were asked to start */
  if (extent->fe_logical < srcoff) {
    extent->fe_length -= srcoff - extent->fe_logical;
    extent->fe_logical = srcoff;
  }

  int64_t written = 0;
  uint64_t i = 0;

  while (i < fiemap->fm_mapped_extents) {
    struct fiemap_extent *next = extent + 1;

    dout(10) << __func__ << " fm_mapped_extents=" << fiemap->fm_mapped_extents
             << " fe_logical=" << extent->fe_logical << " fe_length="
             << extent->fe_length << dendl;

    /* try to merge extents */
    while ((i < fiemap->fm_mapped_extents - 1) &&
           (extent->fe_logical + extent->fe_length == next->fe_logical)) {
        next->fe_length += extent->fe_length;
        next->fe_logical = extent->fe_logical;
        extent = next;
        next = extent + 1;
        i++;
    }

    if (extent->fe_logical + extent->fe_length > srcoff + len)
      extent->fe_length = srcoff + len - extent->fe_logical;

    int64_t actual;

    actual = ::lseek64(from, extent->fe_logical, SEEK_SET);
    if (actual != (int64_t)extent->fe_logical) {
      r = errno;
      derr << "lseek64 to " << srcoff << " got " << cpp_strerror(r) << dendl;
      return r;
    }
    actual = ::lseek64(to, extent->fe_logical - srcoff + dstoff, SEEK_SET);
    if (actual != (int64_t)(extent->fe_logical - srcoff + dstoff)) {
      r = errno;
      derr << "lseek64 to " << dstoff << " got " << cpp_strerror(r) << dendl;
      return r;
    }

    loff_t pos = 0;
    loff_t end = extent->fe_length;
    while (pos < end) {
      int l = MIN(end-pos, buflen);
      r = ::read(from, buf, l);
      dout(25) << "  read from " << pos << "~" << l << " got " << r << dendl;
      if (r < 0) {
        if (errno == EINTR) {
          continue;
        } else {
          r = -errno;
          derr << __func__ << ": read error at " << pos << "~" << len
              << ", " << cpp_strerror(r) << dendl;
          break;
        }
      }
      if (r == 0) {
        r = -ERANGE;
        derr << __func__ << " got short read result at " << pos
             << " of fd " << from << " len " << len << dendl;
        break;
      }
      int op = 0;
      while (op < r) {
        int r2 = safe_write(to, buf+op, r-op);
        dout(25) << " write to " << to << " len " << (r-op)
                 << " got " << r2 << dendl;
        if (r2 < 0) {
          r = r2;
          derr << __func__ << ": write error at " << pos << "~"
               << r-op << ", " << cpp_strerror(r) << dendl;
          break;
        }
        op += (r-op);
      }
      if (r < 0)
        goto out;
      pos += r;
    }
    written += end;
    i++;
    extent++;
  }

  if (r >= 0) {
    if (m_filestore_sloppy_crc) {
      int rc = backend->_crc_update_clone_range(from, to, srcoff, len, dstoff);
      assert(rc >= 0);
    }
    struct stat st;
    r = ::fstat(to, &st);
    if (r < 0) {
      r = -errno;
      derr << __func__ << ": fstat error at " << to << " " << cpp_strerror(r) << dendl;
      goto out;
    }
    if (st.st_size < (int)(dstoff + len)) {
      r = ::ftruncate(to, dstoff + len);
      if (r < 0) {
        r = -errno;
        derr << __func__ << ": ftruncate error at " << dstoff+len << " " << cpp_strerror(r) << dendl;
        goto out;
      }
    }
    r = written;
  }

 out:
  dout(20) << __func__ << " " << srcoff << "~" << len << " to " << dstoff << " = " << r << dendl;
  return r;
}

int XStore::_do_copy_range(int from, int to, uint64_t srcoff, uint64_t len, uint64_t dstoff)
{
  dout(20) << "_do_copy_range " << srcoff << "~" << len << " to " << dstoff << dendl;
  int r = 0;
  int64_t actual;

  actual = ::lseek64(from, srcoff, SEEK_SET);
  if (actual != (int64_t)srcoff) {
    r = errno;
    derr << "lseek64 to " << srcoff << " got " << cpp_strerror(r) << dendl;
    return r;
  }
  actual = ::lseek64(to, dstoff, SEEK_SET);
  if (actual != (int64_t)dstoff) {
    r = errno;
    derr << "lseek64 to " << dstoff << " got " << cpp_strerror(r) << dendl;
    return r;
  }

  loff_t pos = srcoff;
  loff_t end = srcoff + len;
  int buflen = 4096*32;
  char buf[buflen];
  while (pos < end) {
    int l = MIN(end-pos, buflen);
    r = ::read(from, buf, l);
    dout(25) << "  read from " << pos << "~" << l << " got " << r << dendl;
    if (r < 0) {
      if (errno == EINTR) {
	continue;
      } else {
	r = -errno;
	derr << "XStore::_do_copy_range: read error at " << pos << "~" << len
	     << ", " << cpp_strerror(r) << dendl;
	break;
      }
    }
    if (r == 0) {
      // hrm, bad source range, wtf.
      r = -ERANGE;
      derr << "XStore::_do_copy_range got short read result at " << pos
	      << " of fd " << from << " len " << len << dendl;
      break;
    }
    int op = 0;
    while (op < r) {
      int r2 = safe_write(to, buf+op, r-op);
      dout(25) << " write to " << to << " len " << (r-op)
	       << " got " << r2 << dendl;
      if (r2 < 0) {
	r = r2;
	derr << "XStore::_do_copy_range: write error at " << pos << "~"
	     << r-op << ", " << cpp_strerror(r) << dendl;

	break;
      }
      op += (r-op);
    }
    if (r < 0)
      break;
    pos += r;
  }
  if (r >= 0 && m_filestore_sloppy_crc) {
    int rc = backend->_crc_update_clone_range(from, to, srcoff, len, dstoff);
    assert(rc >= 0);
  }
  dout(20) << "_do_copy_range " << srcoff << "~" << len << " to " << dstoff << " = " << r << dendl;
  return r;
}

int XStore::_clone_range(coll_t cid, const ghobject_t& oldoid, const ghobject_t& newoid,
			    uint64_t srcoff, uint64_t len, uint64_t dstoff,
			    const SequencerPosition& spos)
{
  dout(15) << "clone_range " << cid << "/" << oldoid << " -> " << cid << "/" << newoid << " " << srcoff << "~" << len << " to " << dstoff << dendl;

  if (_check_replay_guard(cid, newoid, spos) < 0)
    return 0;

  int r;
  FDRef o, n;
  r = lfn_open(cid, oldoid, false, &o);
  if (r < 0) {
    goto out2;
  }
  r = lfn_open(cid, newoid, true, &n);
  if (r < 0) {
    goto out;
  }
  r = _do_clone_range(**o, **n, srcoff, len, dstoff);
  if (r < 0) {
    r = -errno;
    goto out3;
  }

  // clone is non-idempotent; record our work.
  _set_replay_guard(**n, spos, &newoid);

 out3:
  lfn_close(n);
 out:
  lfn_close(o);
 out2:
  dout(10) << "clone_range " << cid << "/" << oldoid << " -> " << cid << "/" << newoid << " "
	   << srcoff << "~" << len << " to " << dstoff << " = " << r << dendl;
  return r;
}

class SyncEntryTimeout : public Context {
public:
  SyncEntryTimeout(int commit_timeo) 
    : m_commit_timeo(commit_timeo)
  {
  }

  void finish(int r) {
    BackTrace *bt = new BackTrace(1);
    generic_dout(-1) << "XStore: sync_entry timed out after "
	   << m_commit_timeo << " seconds.\n";
    bt->print(*_dout);
    *_dout << dendl;
    delete bt;
    ceph_abort();
  }
private:
  int m_commit_timeo;
};

void XStore::sync_entry()
{
  lock.Lock();
  while (!stop) {
    utime_t max_interval;
    max_interval.set_from_double(m_filestore_max_sync_interval);
    utime_t min_interval;
    min_interval.set_from_double(m_filestore_min_sync_interval);

    utime_t startwait = ceph_clock_now(g_ceph_context);
    if (!force_sync) {
      dout(20) << "sync_entry waiting for max_interval " << max_interval << dendl;
      sync_cond.WaitInterval(g_ceph_context, lock, max_interval);
    } else {
      dout(20) << "sync_entry not waiting, force_sync set" << dendl;
    }

    if (force_sync) {
      dout(20) << "sync_entry force_sync set" << dendl;
      force_sync = false;
    } else {
      // wait for at least the min interval
      utime_t woke = ceph_clock_now(g_ceph_context);
      woke -= startwait;
      dout(20) << "sync_entry woke after " << woke << dendl;
      if (woke < min_interval) {
	utime_t t = min_interval;
	t -= woke;
	dout(20) << "sync_entry waiting for another " << t 
		 << " to reach min interval " << min_interval << dendl;
	sync_cond.WaitInterval(g_ceph_context, lock, t);
      }
    }

    list<Context*> fin;
  again:
    fin.swap(sync_waiters);
    lock.Unlock();
    
    op_tp.pause();
    if (apply_manager.commit_start()) {
      utime_t start = ceph_clock_now(g_ceph_context);
      uint64_t cp = apply_manager.get_committing_seq();

      sync_entry_timeo_lock.Lock();
      SyncEntryTimeout *sync_entry_timeo =
	new SyncEntryTimeout(m_filestore_commit_timeout);
      timer.add_event_after(m_filestore_commit_timeout, sync_entry_timeo);
      sync_entry_timeo_lock.Unlock();

      logger->set(l_os_committing, 1);

      dout(15) << "sync_entry committing " << cp << dendl;
      stringstream errstream;
      if (g_conf->filestore_debug_omap_check && !object_map->check(errstream)) {
	derr << errstream.str() << dendl;
	assert(0);
      }

      if (backend->can_checkpoint()) {
	int err = write_op_seq(op_fd, cp);
	if (err < 0) {
	  derr << "Error during write_op_seq: " << cpp_strerror(err) << dendl;
	  assert(0 == "error during write_op_seq");
	}

	char s[NAME_MAX];
	snprintf(s, sizeof(s), COMMIT_SNAP_ITEM, (long long unsigned)cp);
	uint64_t cid = 0;
	err = backend->create_checkpoint(s, &cid);
	if (err < 0) {
	    int err = errno;
	    derr << "snap create '" << s << "' got error " << err << dendl;
	    assert(err == 0);
	}

	snaps.push_back(cp);
	apply_manager.commit_started();
	op_tp.unpause();

	if (cid > 0) {
	  dout(20) << " waiting for checkpoint " << cid << " to complete" << dendl;
	  err = backend->sync_checkpoint(cid);
	  if (err < 0) {
	    derr << "ioctl WAIT_SYNC got " << cpp_strerror(err) << dendl;
	    assert(0 == "wait_sync got error");
	  }
	  dout(20) << " done waiting for checkpoint" << cid << " to complete" << dendl;
	}
      } else
      {
	apply_manager.commit_started();
	op_tp.unpause();

        int err;
        for (int idx = 0; idx < pgmeta_cache.pgmeta_shards; ++idx) {
          err = pgmeta_cache.submit_shard(idx);
          logger->inc(l_os_omap_cache_shard_flush);
          if (err < 0) {
            derr << "submit omap keys got " << cpp_strerror(err) << dendl;
            assert(0 == "submit_shard returned error");
          }
        }
        object_map->sync();
	err = backend->syncfs();
	if (err < 0) {
	  derr << "syncfs got " << cpp_strerror(err) << dendl;
	  assert(0 == "syncfs returned error");
	}

	err = write_op_seq(op_fd, cp);
	if (err < 0) {
	  derr << "Error during write_op_seq: " << cpp_strerror(err) << dendl;
	  assert(0 == "error during write_op_seq");
	}
	err = ::fsync(op_fd);
	if (err < 0) {
	  derr << "Error during fsync of op_seq: " << cpp_strerror(err) << dendl;
	  assert(0 == "error during fsync of op_seq");
	}
      }
      
      utime_t done = ceph_clock_now(g_ceph_context);
      utime_t lat = done - start;
      utime_t dur = done - startwait;
      dout(10) << "sync_entry commit took " << lat << ", interval was " << dur << dendl;

      logger->inc(l_os_commit);
      logger->tinc(l_os_commit_lat, lat);
      logger->tinc(l_os_commit_len, dur);

      apply_manager.commit_finish();
      for (int i = 0; i < wbthrottle_num; ++i) {
        wbthrottles[i]->clear();
      }

      logger->set(l_os_committing, 0);

      // remove old snaps?
      if (backend->can_checkpoint()) {
	char s[NAME_MAX];
	while (snaps.size() > 2) {
	  snprintf(s, sizeof(s), COMMIT_SNAP_ITEM, (long long unsigned)snaps.front());
	  snaps.pop_front();
	  dout(10) << "removing snap '" << s << "'" << dendl;
	  int r = backend->destroy_checkpoint(s);
	  if (r) {
	    int err = errno;
	    derr << "unable to destroy snap '" << s << "' got " << cpp_strerror(err) << dendl;
	  }
	}
      }

      dout(15) << "sync_entry committed to op_seq " << cp << dendl;

      sync_entry_timeo_lock.Lock();
      timer.cancel_event(sync_entry_timeo);
      sync_entry_timeo_lock.Unlock();
    } else {
      op_tp.unpause();
      uint64_t cp = apply_manager.get_committing_seq();
      int err = write_op_seq(op_fd, cp);
      if (err < 0) {
        derr << "Error during write_op_seq: " << cpp_strerror(err) << dendl;
        assert(0 == "error during write_op_seq");
      }
    }
    
    lock.Lock();
    finish_contexts(g_ceph_context, fin, 0);
    fin.clear();
    if (!sync_waiters.empty()) {
      dout(10) << "sync_entry more waiters, committing again" << dendl;
      goto again;
    }
    if (!stop && journal && journal->should_commit_now()) {
      dout(10) << "sync_entry journal says we should commit again (probably is/was full)" << dendl;
      goto again;
    }
  }
  stop = false;
  lock.Unlock();
}

void XStore::_start_sync()
{
  if (!journal) {  // don't do a big sync if the journal is on
    dout(10) << "start_sync" << dendl;
    sync_cond.Signal();
  } else {
    dout(10) << "start_sync - NOOP (journal is on)" << dendl;
  }
}

void XStore::do_force_sync()
{
  dout(10) << __func__ << dendl;
  Mutex::Locker l(lock);
  force_sync = true;
  sync_cond.Signal();
}

void XStore::start_sync(Context *onsafe)
{
  Mutex::Locker l(lock);
  sync_waiters.push_back(onsafe);
  sync_cond.Signal();
  dout(10) << "start_sync" << dendl;
}

void XStore::sync()
{
  Mutex l("XStore::sync");
  Cond c;
  bool done;
  C_SafeCond *fin = new C_SafeCond(&l, &c, &done);

  start_sync(fin);

  l.Lock();
  while (!done) {
    dout(10) << "sync waiting" << dendl;
    c.Wait(l);
  }
  l.Unlock();
  dout(10) << "sync done" << dendl;
}

void XStore::_flush_op_queue()
{
  dout(10) << "_flush_op_queue draining op tp" << dendl;
  op_wq.drain();
  dout(10) << "_flush_op_queue waiting for apply finisher" << dendl;
  for (vector<Finisher*>::iterator it = ondisk_finishers.begin(); it != ondisk_finishers.end(); ++it) {
    (*it)->wait_for_empty();
  }
  for (vector<Finisher*>::iterator it = apply_finishers.begin(); it != apply_finishers.end(); ++it) {
    (*it)->wait_for_empty();
  }
}

/*
 * flush - make every queued write readable
 */
void XStore::flush()
{
  dout(10) << "flush" << dendl;

  if (g_conf->filestore_blackhole) {
    // wait forever
    Mutex lock("XStore::flush::lock");
    Cond cond;
    lock.Lock();
    while (true)
      cond.Wait(lock);
    assert(0);
  }
 
  if (journal)
    journal->flush();
  dout(10) << "flush draining ondisk finisher" << dendl;
  for (vector<Finisher*>::iterator it = ondisk_finishers.begin(); it != ondisk_finishers.end(); ++it) {
    (*it)->wait_for_empty();
  }
  for (vector<Finisher*>::iterator it = apply_finishers.begin(); it != apply_finishers.end(); ++it) {
    (*it)->wait_for_empty();
  }

  _flush_op_queue();
  dout(10) << "flush complete" << dendl;
}

/*
 * sync_and_flush - make every queued write readable AND committed to disk
 */
void XStore::sync_and_flush()
{
  dout(10) << "sync_and_flush" << dendl;

  if (journal)
    journal->flush();
  _flush_op_queue();

  dout(10) << "sync_and_flush done" << dendl;
}

int XStore::snapshot(const string& name)
{
  dout(10) << "snapshot " << name << dendl;
  sync_and_flush();

  if (!backend->can_checkpoint()) {
    dout(0) << "snapshot " << name << " failed, not supported" << dendl;
    return -EOPNOTSUPP;
  }

  char s[NAME_MAX];
  snprintf(s, sizeof(s), CLUSTER_SNAP_ITEM, name.c_str());

  int r = backend->create_checkpoint(s, NULL);
  if (r) {
    r = -errno;
    derr << "snapshot " << name << " failed: " << cpp_strerror(r) << dendl;
  }

  return r;
}

// -------------------------------
// attributes

int XStore::_fgetattr(int fd, const char *name, bufferptr& bp, int *chunks)
{
  char val[CHAIN_XATTR_MAX_BLOCK_LEN];
  int l = chain_fgetxattr(fd, name, val, sizeof(val), chunks);
  if (l >= 0) {
    bp = buffer::create(l);
    memcpy(bp.c_str(), val, l);
  } else if (l == -ERANGE) {
    l = chain_fgetxattr(fd, name, 0, 0);
    if (l > 0) {
      bp = buffer::create(l);
      l = chain_fgetxattr(fd, name, bp.c_str(), l, chunks);
    }
  }
  assert(!m_filestore_fail_eio || l != -EIO);
  return l;
}

int XStore::_fgetattrs(int fd, map<string, pair<bufferptr, int> >& aset)
{
  // get attr list
  char names1[100];
  int len = chain_flistxattr(fd, names1, sizeof(names1)-1);
  char *names2 = 0;
  char *name = 0;
  if (len == -ERANGE) {
    len = chain_flistxattr(fd, 0, 0);
    if (len < 0) {
      assert(!m_filestore_fail_eio || len != -EIO);
      return len;
    }
    dout(10) << " -ERANGE, len is " << len << dendl;
    names2 = new char[len+1];
    len = chain_flistxattr(fd, names2, len);
    dout(10) << " -ERANGE, got " << len << dendl;
    if (len < 0) {
      assert(!m_filestore_fail_eio || len != -EIO);
      return len;
    }
    name = names2;
  } else if (len < 0) {
    assert(!m_filestore_fail_eio || len != -EIO);
    return len;
  } else {
    name = names1;
  }
  name[len] = 0;

  char *end = name + len;
  while (name < end) {
    char *attrname = name;
    if (parse_attrname(&name)) {
      if (*name) {
        dout(20) << "fgetattrs " << fd << " getting '" << name << "'" << dendl;
        int r = _fgetattr(fd, attrname, aset[name].first, &aset[name].second);
        if (r < 0)
	  return r;
      }
    }
    name += strlen(name) + 1;
  }

  delete[] names2;
  return 0;
}

int XStore::_fgetattrs_chunks(int fd, map<string, int>& aset)
{
  // get attr list
  char names1[CHAIN_XATTR_MAX_NAME_LEN];
  int len = chain_flistxattr(fd, names1, sizeof(names1)-1, &aset);
  char *names2 = 0;
  if (len == -ERANGE) {
    len = chain_flistxattr(fd, 0, 0);
    if (len < 0) {
      assert(!m_filestore_fail_eio || len != -EIO);
      return len;
    }
    dout(10) << " -ERANGE, len is " << len << dendl;
    names2 = new char[len+1];
    len = chain_flistxattr(fd, names2, len, &aset);
    dout(10) << " -ERANGE, got " << len << dendl;
    if (len < 0) {
      assert(!m_filestore_fail_eio || len != -EIO);
      delete[] names2;
      return len;
    }
  } else if (len < 0) {
    assert(!m_filestore_fail_eio || len != -EIO);
    return len;
  }
  delete[] names2;
  return 0;
}

int XStore::_fsetattr(int fd, const string& name, bufferptr& bp, int chunks)
{
  char n[CHAIN_XATTR_MAX_NAME_LEN];
  get_attrname(name.c_str(), n, CHAIN_XATTR_MAX_NAME_LEN);
  // ??? Why do we skip setting all the other attrs if one fails?
  const char *val;
  if (bp.length())
    val = bp.c_str();
  else
    val = "";
  int r = chain_fsetxattr(fd, n, val, bp.length(), chunks);
  if (r < 0) {
    derr << "XStore::_setattrs: chain_setxattr returned " << r << dendl;
    return r;
  }
  return 0;
}

// debug EIO injection
void XStore::inject_data_error(const ghobject_t &oid) {
  Mutex::Locker l(read_error_lock);
  dout(10) << __func__ << ": init error on " << oid << dendl;
  data_error_set.insert(oid);
}
void XStore::inject_mdata_error(const ghobject_t &oid) {
  Mutex::Locker l(read_error_lock);
  dout(10) << __func__ << ": init error on " << oid << dendl;
  mdata_error_set.insert(oid);
}
void XStore::debug_obj_on_delete(const ghobject_t &oid) {
  Mutex::Locker l(read_error_lock);
  dout(10) << __func__ << ": clear error on " << oid << dendl;
  data_error_set.erase(oid);
  mdata_error_set.erase(oid);
}
bool XStore::debug_data_eio(const ghobject_t &oid) {
  Mutex::Locker l(read_error_lock);
  if (data_error_set.count(oid)) {
    dout(10) << __func__ << ": inject error on " << oid << dendl;
    return true;
  } else {
    return false;
  }
}
bool XStore::debug_mdata_eio(const ghobject_t &oid) {
  Mutex::Locker l(read_error_lock);
  if (mdata_error_set.count(oid)) {
    dout(10) << __func__ << ": inject error on " << oid << dendl;
    return true;
  } else {
    return false;
  }
}


// objects

int XStore::getattr(coll_t cid, const ghobject_t& oid, const char *name, bufferptr &bp)
{
  tracepoint(objectstore, getattr_enter, cid.c_str());
  dout(15) << "getattr " << cid << "/" << oid << " '" << name << "'" << dendl;
  FDRef fd;
  int r = lfn_open(cid, oid, false, &fd);
  if (r < 0) {
    goto out;
  }
  char n[CHAIN_XATTR_MAX_NAME_LEN];
  get_attrname(name, n, CHAIN_XATTR_MAX_NAME_LEN);
  r = _fgetattr(**fd, n, bp);
  lfn_close(fd);
  if (r == -ENODATA) {
    map<string, bufferlist> got;
    set<string> to_get;
    to_get.insert(string(name));
    Index index;
    r = get_index(cid, &index);
    if (r < 0) {
      dout(10) << __func__ << " could not get index r = " << r << dendl;
      goto out;
    }
    r = object_map->get_xattrs(oid, to_get, &got);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " get_xattrs err r =" << r << dendl;
      goto out;
    }
    if (got.empty()) {
      dout(10) << __func__ << " got.size() is 0" << dendl;
      return -ENODATA;
    }
    bp = bufferptr(got.begin()->second.c_str(),
		   got.begin()->second.length());
    r = bp.length();
  }
 out:
  dout(10) << "getattr " << cid << "/" << oid << " '" << name << "' = " << r << dendl;
  assert(!m_filestore_fail_eio || r != -EIO);
  if (g_conf->filestore_debug_inject_read_err &&
      debug_mdata_eio(oid)) {
    return -EIO;
  } else {
    tracepoint(objectstore, getattr_exit, r);
    return r;
  }
}

int XStore::getattrs(coll_t cid, const ghobject_t& oid, map<string,bufferptr>& aset)
{
  tracepoint(objectstore, getattrs_enter, cid.c_str());
  set<string> omap_attrs;
  map<string, bufferlist> omap_aset;
  map<string, pair<bufferptr, int> > orig_set;
  Index index;
  dout(15) << "getattrs " << cid << "/" << oid << dendl;
  FDRef fd;
  bool spill_out = true;
  char buf[2];

  int r = lfn_open(cid, oid, false, &fd);
  if (r < 0) {
    goto out;
  }

  r = chain_fgetxattr(**fd, XATTR_SPILL_OUT_NAME, buf, sizeof(buf));
  if (r >= 0 && !strncmp(buf, XATTR_NO_SPILL_OUT, sizeof(XATTR_NO_SPILL_OUT)))
    spill_out = false;

  r = _fgetattrs(**fd, orig_set);
  if (r < 0) {
    goto out;
  }
  for (map<string, pair<bufferptr, int> >::iterator it = orig_set.begin();
       it != orig_set.end();
       ++it) {
    aset[it->first] = it->second.first;
  }
  lfn_close(fd);

  if (!spill_out) {
    dout(10) << __func__ << " no xattr exists in object_map r = " << r << dendl;
    goto out;
  }

  r = get_index(cid, &index);
  if (r < 0) {
    dout(10) << __func__ << " could not get index r = " << r << dendl;
    goto out;
  }
  {
    r = object_map->get_all_xattrs(oid, &omap_attrs);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " could not get omap_attrs r = " << r << dendl;
      goto out;
    }

    r = object_map->get_xattrs(oid, omap_attrs, &omap_aset);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " could not get omap_attrs r = " << r << dendl;
      goto out;
    }
    if (r == -ENOENT)
      r = 0;
  }
  assert(omap_attrs.size() == omap_aset.size());
  for (map<string, bufferlist>::iterator i = omap_aset.begin();
	 i != omap_aset.end();
	 ++i) {
    string key(i->first);
    aset.insert(make_pair(key,
			    bufferptr(i->second.c_str(), i->second.length())));
  }
 out:
  dout(10) << "getattrs " << cid << "/" << oid << " = " << r << dendl;
  assert(!m_filestore_fail_eio || r != -EIO);

  if (g_conf->filestore_debug_inject_read_err &&
      debug_mdata_eio(oid)) {
    return -EIO;
  } else {
    tracepoint(objectstore, getattrs_exit, r);
    return r;
  }
}

int XStore::_setattrs(coll_t cid, const ghobject_t& oid, map<string,bufferptr>& aset,
			 const SequencerPosition &spos)
{
  map<string, bufferlist> omap_set;
  set<string> omap_remove;
  map<string, int> inline_set;
  FDRef fd;
  int spill_out = -1;
  bool incomplete_inline = false;

  int r = lfn_open(cid, oid, false, &fd);
  if (r < 0) {
    goto out;
  }

  char buf[2];
  r = chain_fgetxattr(**fd, XATTR_SPILL_OUT_NAME, buf, sizeof(buf));
  if (r >= 0 && !strncmp(buf, XATTR_NO_SPILL_OUT, sizeof(XATTR_NO_SPILL_OUT)))
    spill_out = 0;
  else
    spill_out = 1;

  r = _fgetattrs_chunks(**fd, inline_set);
  incomplete_inline = (r == -E2BIG);
  assert(!m_filestore_fail_eio || r != -EIO);
  dout(15) << "setattrs " << cid << "/" << oid
    	   << (incomplete_inline ? " (incomplete_inline, forcing omap)" : "")
	   << dendl;

  for (map<string,bufferptr>::iterator p = aset.begin();
       p != aset.end();
       ++p) {
    char n[CHAIN_XATTR_MAX_NAME_LEN];
    get_attrname(p->first.c_str(), n, CHAIN_XATTR_MAX_NAME_LEN);

    if (incomplete_inline) {
      chain_fremovexattr(**fd, n); // ignore any error
      omap_set[p->first].push_back(p->second);
      continue;
    }

    if (p->second.length() > m_filestore_max_inline_xattr_size) {
	if (inline_set.count(p->first)) {
	  inline_set.erase(p->first);
	  r = chain_fremovexattr(**fd, n);
	  if (r < 0)
	    goto out_close;
	}
	omap_set[p->first].push_back(p->second);
	continue;
    }

    if (!inline_set.count(p->first) &&
	  inline_set.size() >= m_filestore_max_inline_xattrs) {
	omap_set[p->first].push_back(p->second);
	continue;
    }
    if (spill_out)
      omap_remove.insert(p->first);
    map<string, int>::iterator iter_inline = inline_set.find(p->first);
    if (iter_inline != inline_set.end() && iter_inline->second > 0) {
      r = _fsetattr(**fd, p->first, p->second, iter_inline->second);
    } else {
      // this is new attr, we do not know the number of chunk for this attr
      inline_set.insert(make_pair(p->first, -1));
      r = _fsetattr(**fd, p->first, p->second, -1);
    }
    if (r < 0)
      goto out_close;
  }

  if (spill_out != 1 && !omap_set.empty()) {
    chain_fsetxattr(**fd, XATTR_SPILL_OUT_NAME, XATTR_SPILL_OUT,
		    sizeof(XATTR_SPILL_OUT));
  }

  if (spill_out && !omap_remove.empty()) {
    r = object_map->remove_xattrs(oid, omap_remove, &spos);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " could not remove_xattrs r = " << r << dendl;
      assert(!m_filestore_fail_eio || r != -EIO);
      goto out_close;
    } else {
      r = 0; // don't confuse the debug output
    }
  }

  if (!omap_set.empty()) {
    r = object_map->set_xattrs(oid, omap_set, &spos);
    if (r < 0) {
      dout(10) << __func__ << " could not set_xattrs r = " << r << dendl;
      assert(!m_filestore_fail_eio || r != -EIO);
      goto out_close;
    }
  }
 out_close:
  lfn_close(fd);
 out:
  dout(10) << "setattrs " << cid << "/" << oid << " = " << r << dendl;
  return r;
}


int XStore::_rmattr(coll_t cid, const ghobject_t& oid, const char *name,
		       const SequencerPosition &spos)
{
  dout(15) << "rmattr " << cid << "/" << oid << " '" << name << "'" << dendl;
  FDRef fd;
  bool spill_out = true;
  bufferptr bp;

  int r = lfn_open(cid, oid, false, &fd);
  if (r < 0) {
    goto out;
  }

  char buf[2];
  r = chain_fgetxattr(**fd, XATTR_SPILL_OUT_NAME, buf, sizeof(buf));
  if (r >= 0 && !strncmp(buf, XATTR_NO_SPILL_OUT, sizeof(XATTR_NO_SPILL_OUT))) {
    spill_out = false;
  }

  char n[CHAIN_XATTR_MAX_NAME_LEN];
  get_attrname(name, n, CHAIN_XATTR_MAX_NAME_LEN);
  r = chain_fremovexattr(**fd, n);
  if (r == -ENODATA && spill_out) {
    Index index;
    r = get_index(cid, &index);
    if (r < 0) {
      dout(10) << __func__ << " could not get index r = " << r << dendl;
      goto out_close;
    }
    set<string> to_remove;
    to_remove.insert(string(name));
    r = object_map->remove_xattrs(oid, to_remove, &spos);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " could not remove_xattrs index r = " << r << dendl;
      assert(!m_filestore_fail_eio || r != -EIO);
      goto out_close;
    }
  }
 out_close:
  lfn_close(fd);
 out:
  dout(10) << "rmattr " << cid << "/" << oid << " '" << name << "' = " << r << dendl;
  return r;
}

int XStore::_rmattrs(coll_t cid, const ghobject_t& oid,
			const SequencerPosition &spos)
{
  dout(15) << "rmattrs " << cid << "/" << oid << dendl;

  map<string, int> aset;
  FDRef fd;
  set<string> omap_attrs;
  Index index;
  bool spill_out = true;

  int r = lfn_open(cid, oid, false, &fd);
  if (r < 0) {
    goto out;
  }

  char buf[2];
  r = chain_fgetxattr(**fd, XATTR_SPILL_OUT_NAME, buf, sizeof(buf));
  if (r >= 0 && !strncmp(buf, XATTR_NO_SPILL_OUT, sizeof(XATTR_NO_SPILL_OUT))) {
    spill_out = false;
  }

  r = _fgetattrs_chunks(**fd, aset);
  if (r >= 0) {
    for (map<string, int>::iterator p = aset.begin(); p != aset.end(); ++p) {
      char n[CHAIN_XATTR_MAX_NAME_LEN];
      get_attrname(p->first.c_str(), n, CHAIN_XATTR_MAX_NAME_LEN);
      r = chain_fremovexattr(**fd, n);
      if (r < 0)
	break;
    }
  }

  if (!spill_out) {
    dout(10) << __func__ << " no xattr exists in object_map r = " << r << dendl;
    goto out_close;
  }

  r = get_index(cid, &index);
  if (r < 0) {
    dout(10) << __func__ << " could not get index r = " << r << dendl;
    goto out_close;
  }
  {
    r = object_map->get_all_xattrs(oid, &omap_attrs);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " could not get omap_attrs r = " << r << dendl;
      assert(!m_filestore_fail_eio || r != -EIO);
      goto out_close;
    }
    r = object_map->remove_xattrs(oid, omap_attrs, &spos);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " could not remove omap_attrs r = " << r << dendl;
      goto out_close;
    }
    if (r == -ENOENT)
      r = 0;
    chain_fsetxattr(**fd, XATTR_SPILL_OUT_NAME, XATTR_NO_SPILL_OUT,
		  sizeof(XATTR_NO_SPILL_OUT));
  }

 out_close:
  lfn_close(fd);
 out:
  dout(10) << "rmattrs " << cid << "/" << oid << " = " << r << dendl;
  return r;
}



// collections

int XStore::collection_getattr(coll_t c, const char *name,
				  void *value, size_t size) 
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "collection_getattr " << fn << " '" << name << "' len " << size << dendl;
  int r;
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    r = -errno;
    goto out;
  }
  char n[PATH_MAX];
  get_attrname(name, n, PATH_MAX);
  r = chain_fgetxattr(fd, n, value, size);
  VOID_TEMP_FAILURE_RETRY(::close(fd));
 out:
  dout(10) << "collection_getattr " << fn << " '" << name << "' len " << size << " = " << r << dendl;
  assert(!m_filestore_fail_eio || r != -EIO);
  return r;
}

int XStore::collection_getattr(coll_t c, const char *name, bufferlist& bl)
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "collection_getattr " << fn << " '" << name << "'" << dendl;
  char n[PATH_MAX];
  get_attrname(name, n, PATH_MAX);
  buffer::ptr bp;
  int r;
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    r = -errno;
    goto out;
  }
  r = _fgetattr(fd, n, bp);
  bl.push_back(bp);
  VOID_TEMP_FAILURE_RETRY(::close(fd));
 out:
  dout(10) << "collection_getattr " << fn << " '" << name << "' = " << r << dendl;
  assert(!m_filestore_fail_eio || r != -EIO);
  return r;
}

int XStore::collection_getattrs(coll_t cid, map<string,bufferptr>& aset) 
{
  char fn[PATH_MAX];
  get_cdir(cid, fn, sizeof(fn));
  map<string, pair<bufferptr, int> > orig_aset;
  dout(10) << "collection_getattrs " << fn << dendl;
  int r = 0;
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    r = -errno;
    goto out;
  }
  r = _fgetattrs(fd, orig_aset);
  for (map<string, pair<bufferptr, int> >::iterator it = orig_aset.begin();
       it != orig_aset.end();
       ++it) {
    aset[it->first] = it->second.first;
  }
  VOID_TEMP_FAILURE_RETRY(::close(fd));
 out:
  dout(10) << "collection_getattrs " << fn << " = " << r << dendl;
  assert(!m_filestore_fail_eio || r != -EIO);
  return r;
}


int XStore::_collection_setattr(coll_t c, const char *name,
				  const void *value, size_t size) 
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(10) << "collection_setattr " << fn << " '" << name << "' len " << size << dendl;
  char n[PATH_MAX];
  int r;
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    r = -errno;
    goto out;
  }
  get_attrname(name, n, PATH_MAX);
  r = chain_fsetxattr(fd, n, value, size);
  VOID_TEMP_FAILURE_RETRY(::close(fd));
 out:
  dout(10) << "collection_setattr " << fn << " '" << name << "' len " << size << " = " << r << dendl;
  return r;
}

int XStore::_collection_rmattr(coll_t c, const char *name) 
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "collection_rmattr " << fn << dendl;
  char n[PATH_MAX];
  get_attrname(name, n, PATH_MAX);
  int r;
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    r = -errno;
    goto out;
  }
  r = chain_fremovexattr(fd, n);
  VOID_TEMP_FAILURE_RETRY(::close(fd));
 out:
  dout(10) << "collection_rmattr " << fn << " = " << r << dendl;
  return r;
}


int XStore::_collection_setattrs(coll_t cid, map<string,bufferptr>& aset) 
{
  char fn[PATH_MAX];
  get_cdir(cid, fn, sizeof(fn));
  dout(15) << "collection_setattrs " << fn << dendl;
  int r = 0;
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    r = -errno;
    goto out;
  }
  for (map<string,bufferptr>::iterator p = aset.begin();
       p != aset.end();
       ++p) {
    char n[PATH_MAX];
    get_attrname(p->first.c_str(), n, PATH_MAX);
    r = chain_fsetxattr(fd, n, p->second.c_str(), p->second.length());
    if (r < 0)
      break;
  }
  VOID_TEMP_FAILURE_RETRY(::close(fd));
 out:
  dout(10) << "collection_setattrs " << fn << " = " << r << dendl;
  return r;
}

int XStore::_collection_remove_recursive(const coll_t &cid,
					    const SequencerPosition &spos, int osr)
{
  struct stat st;
  int r = collection_stat(cid, &st);
  if (r < 0) {
    if (r == -ENOENT)
      return 0;
    return r;
  }

  vector<ghobject_t> objects;
  ghobject_t max;
  while (!max.is_max()) {
    r = collection_list_partial(cid, max, 200, 300, 0, &objects, &max);
    if (r < 0)
      return r;
    for (vector<ghobject_t>::iterator i = objects.begin();
	 i != objects.end();
	 ++i) {
      assert(_check_replay_guard(cid, *i, spos));
      r = _remove(cid, *i, spos, osr);
      if (r < 0)
	return r;
    }
  }
  return _destroy_collection(cid);
}

// --------------------------
// collections

int XStore::collection_version_current(coll_t c, uint32_t *version)
{
  Index index;
  int r = get_index(c, &index);
  if (r < 0)
    return r;

  assert(NULL != index.index);
  RWLock::RLocker l((index.index)->access_lock);

  *version = index->collection_version();
  if (*version == target_version)
    return 1;
  else 
    return 0;
}

int XStore::list_collections(vector<coll_t>& ls) 
{
  tracepoint(objectstore, list_collections_enter);
  dout(10) << "list_collections" << dendl;

  char fn[PATH_MAX];
  snprintf(fn, sizeof(fn), "%s/current", basedir.c_str());

  int r = 0;
  DIR *dir = ::opendir(fn);
  if (!dir) {
    r = -errno;
    derr << "tried opening directory " << fn << ": " << cpp_strerror(-r) << dendl;
    assert(!m_filestore_fail_eio || r != -EIO);
    return r;
  }

  char buf[offsetof(struct dirent, d_name) + PATH_MAX + 1];
  struct dirent *de;
  while ((r = ::readdir_r(dir, (struct dirent *)&buf, &de)) == 0) {
    if (!de)
      break;
    if (de->d_type == DT_UNKNOWN) {
      // d_type not supported (non-ext[234], btrfs), must stat
      struct stat sb;
      char filename[PATH_MAX];
      snprintf(filename, sizeof(filename), "%s/%s", fn, de->d_name);

      r = ::stat(filename, &sb);
      if (r < 0) {
	r = -errno;
	derr << "stat on " << filename << ": " << cpp_strerror(-r) << dendl;
	assert(!m_filestore_fail_eio || r != -EIO);
	break;
      }
      if (!S_ISDIR(sb.st_mode)) {
	continue;
      }
    } else if (de->d_type != DT_DIR) {
      continue;
    }
    if (strcmp(de->d_name, "omap") == 0) {
      continue;
    }
    if (de->d_name[0] == '.' &&
	(de->d_name[1] == '\0' ||
	 (de->d_name[1] == '.' &&
	  de->d_name[2] == '\0')))
      continue;
    ls.push_back(coll_t(de->d_name));
  }

  if (r > 0) {
    derr << "trying readdir_r " << fn << ": " << cpp_strerror(r) << dendl;
    r = -r;
  }

  ::closedir(dir);
  assert(!m_filestore_fail_eio || r != -EIO);
  tracepoint(objectstore, list_collections_exit, r);
  return r;
}

int XStore::collection_stat(coll_t c, struct stat *st) 
{
  tracepoint(objectstore, collection_stat_enter, c.c_str());
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "collection_stat " << fn << dendl;
  int r = ::stat(fn, st);
  if (r < 0)
    r = -errno;
  dout(10) << "collection_stat " << fn << " = " << r << dendl;
  assert(!m_filestore_fail_eio || r != -EIO);
  tracepoint(objectstore, collection_stat_exit, r);
  return r;
}

bool XStore::collection_exists(coll_t c) 
{
  tracepoint(objectstore, collection_exists_enter, c.c_str());
  struct stat st;
  bool ret = collection_stat(c, &st) == 0;
  tracepoint(objectstore, collection_exists_exit, ret);
  return ret;
}

bool XStore::collection_empty(coll_t c) 
{  
  tracepoint(objectstore, collection_empty_enter, c.c_str());
  dout(15) << "collection_empty " << c << dendl;
  Index index;
  int r = get_index(c, &index);
  if (r < 0)
    return false;

  assert(NULL != index.index);
  RWLock::RLocker l((index.index)->access_lock);

  vector<ghobject_t> ls;
  collection_list_handle_t handle;
  r = index->collection_list_partial(ghobject_t(), 1, 1, 0, &ls, NULL);
  if (r < 0) {
    assert(!m_filestore_fail_eio || r != -EIO);
    return false;
  }
  bool ret = ls.empty();
  tracepoint(objectstore, collection_empty_exit, ret);
  return ret;
}

int XStore::collection_list_range(coll_t c, ghobject_t start, ghobject_t end,
                                     snapid_t seq, vector<ghobject_t> *ls)
{
  tracepoint(objectstore, collection_list_range_enter, c.c_str());
  bool done = false;
  ghobject_t next = start;

  while (!done) {
    vector<ghobject_t> next_objects;
    int r = collection_list_partial(c, next,
                                get_ideal_list_min(), get_ideal_list_max(),
                                seq, &next_objects, &next);
    if (r < 0)
      return r;

    ls->insert(ls->end(), next_objects.begin(), next_objects.end());

    // special case for empty collection
    if (ls->empty()) {
      break;
    }

    while (!ls->empty() && ls->back() >= end) {
      ls->pop_back();
      done = true;
    }

    if (next >= end) {
      done = true;
    }
  }

  tracepoint(objectstore, collection_list_range_exit, 0);
  return 0;
}

int XStore::collection_list_partial(coll_t c, ghobject_t start,
				       int min, int max, snapid_t seq,
				       vector<ghobject_t> *ls, ghobject_t *next)
{
  tracepoint(objectstore, collection_list_partial_enter, c.c_str());
  dout(10) << "collection_list_partial: " << c << dendl;
  Index index;
  int r = get_index(c, &index);
  if (r < 0)
    return r;

  assert(NULL != index.index);
  RWLock::RLocker l((index.index)->access_lock);

  r = index->collection_list_partial(start,
				     min, max, seq,
				     ls, next);
  if (r < 0) {
    assert(!m_filestore_fail_eio || r != -EIO);
    return r;
  }
  if (ls)
    dout(20) << "objects: " << *ls << dendl;
  tracepoint(objectstore, collection_list_partial_exit, 0);
  return 0;
}

int XStore::collection_list(coll_t c, vector<ghobject_t>& ls)
{  
  tracepoint(objectstore, collection_list_enter, c.c_str());
  Index index;
  int r = get_index(c, &index);
  if (r < 0)
    return r;

  assert(NULL != index.index);
  RWLock::RLocker l((index.index)->access_lock);

  r = index->collection_list(&ls);
  assert(!m_filestore_fail_eio || r != -EIO);
  tracepoint(objectstore, collection_list_exit, r);
  return r;
}

int XStore::omap_get(coll_t c, const ghobject_t &hoid,
			bufferlist *header,
			map<string, bufferlist> *out)
{
  tracepoint(objectstore, omap_get_enter, c.c_str());
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  int r = object_map->get(hoid, header, out);
  if (r == -ENOENT) {
    Index index;
    r = get_index(c, &index);
    if (r < 0)
      return r;
    {
      assert(NULL != index.index);
      RWLock::RLocker l((index.index)->access_lock);
      r = lfn_find(hoid, index);
      if (r < 0)
        return r;
    }
  } else if (r < 0) {
    assert(!m_filestore_fail_eio || r != -EIO);
    return r;
  }
  if (hoid.is_pgmeta()) {
    pgmeta_cache.get_all(hoid, NULL, out);
  }
  tracepoint(objectstore, omap_get_exit, 0);
  return 0;
}

int XStore::omap_get_header(
  coll_t c,
  const ghobject_t &hoid,
  bufferlist *bl,
  bool allow_eio)
{
  tracepoint(objectstore, omap_get_header_enter, c.c_str());
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  int r = object_map->get_header(hoid, bl);
  if (r == -ENOENT) {
    Index index;
    r = get_index(c, &index);
    if (r < 0)
      return r;
    {
      assert(NULL != index.index);
      RWLock::RLocker l((index.index)->access_lock);
      r = lfn_find(hoid, index);
      if (r < 0)
        return r;
    }
  } else if (r < 0) {
    assert(allow_eio || !m_filestore_fail_eio || r != -EIO);
    return r;
  }
  tracepoint(objectstore, omap_get_header_exit, 0);
  return 0;
}

int XStore::omap_get_keys(coll_t c, const ghobject_t &hoid, set<string> *keys)
{
  tracepoint(objectstore, omap_get_keys_enter, c.c_str());
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  int r = object_map->get_keys(hoid, keys);
  if (r == -ENOENT) {
    Index index;
    r = get_index(c, &index);
    if (r < 0)
      return r;
    {
      assert(NULL != index.index);
      RWLock::RLocker l((index.index)->access_lock);
      r = lfn_find(hoid, index);
      if (r < 0)
        return r;
    }
  } else if (r < 0) {
    assert(!m_filestore_fail_eio || r != -EIO);
    return r;
  }
  if (hoid.is_pgmeta()) {
    pgmeta_cache.get_all(hoid, keys, NULL);
  }
  tracepoint(objectstore, omap_get_keys_exit, 0);
  return 0;
}

int XStore::omap_get_values(coll_t c, const ghobject_t &hoid,
			       const set<string> &keys,
			       map<string, bufferlist> *out)
{
  tracepoint(objectstore, omap_get_values_enter, c.c_str());
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  int r = object_map->get_values(hoid, keys, out);
  if (r == -ENOENT) {
    Index index;
    r = get_index(c, &index);
    if (r < 0)
      return r;
    {
      assert(NULL != index.index);
      RWLock::RLocker l((index.index)->access_lock);
      r = lfn_find(hoid, index);
      if (r < 0)
        return r;
    }
  } else if (r < 0) {
    assert(!m_filestore_fail_eio || r != -EIO);
    return r;
  }
  if (hoid.is_pgmeta()) {
    pgmeta_cache.get_by_keys(hoid, keys, NULL, out);
  }
  tracepoint(objectstore, omap_get_values_exit, 0);
  return 0;
}

int XStore::omap_check_keys(coll_t c, const ghobject_t &hoid,
			       const set<string> &keys,
			       set<string> *out)
{
  tracepoint(objectstore, omap_check_keys_enter, c.c_str());
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  int r = object_map->check_keys(hoid, keys, out);
  if (r == -ENOENT) {
    Index index;
    r = get_index(c, &index);
    if (r < 0)
      return r;
    {
      assert(NULL != index.index);
      RWLock::RLocker l((index.index)->access_lock);
      r = lfn_find(hoid, index);
      if (r < 0)
        return r;
    }
  } else if (r < 0) {
    assert(!m_filestore_fail_eio || r != -EIO);
    return r;
  }
  if (hoid.is_pgmeta()) {
    pgmeta_cache.get_by_keys(hoid, keys, out, NULL);
  }
  tracepoint(objectstore, omap_check_keys_exit, 0);
  return 0;
}

ObjectMap::ObjectMapIterator XStore::get_omap_iterator(coll_t c,
							  const ghobject_t &hoid)
{
  tracepoint(objectstore, get_omap_iterator, c.c_str());
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  Index index;
  int r = get_index(c, &index);
  if (r < 0)
    return ObjectMap::ObjectMapIterator(); 
  {
    assert(NULL != index.index);
    RWLock::RLocker l((index.index)->access_lock);
    r = lfn_find(hoid, index);
    if (r < 0)
      return ObjectMap::ObjectMapIterator();
  }
  if (hoid.is_pgmeta()) {
    if (pgmeta_cache.submit_pgmeta_keys(hoid) < 0)
      return ObjectMap::ObjectMapIterator();
  }
  return object_map->get_iterator(hoid);
}

int XStore::_collection_hint_expected_num_objs(coll_t c, uint32_t pg_num,
    uint64_t expected_num_objs,
    const SequencerPosition &spos)
{
  dout(15) << __func__ << " collection: " << c << " pg number: "
     << pg_num << " expected number of objects: " << expected_num_objs << dendl;

  if (!collection_empty(c) && !replaying) {
    dout(0) << "Failed to give an expected number of objects hint to collection : "
      << c << ", only empty collection can take such type of hint. " << dendl;
    return 0;
  }

  int ret;
  Index index;
  ret = get_index(c, &index);
  if (ret < 0)
    return ret;
  // Pre-hash the collection
  ret = index->pre_hash_collection(pg_num, expected_num_objs);
  dout(10) << "pre_hash_collection " << c << " = " << ret << dendl;
  if (ret < 0)
    return ret;
  _set_replay_guard(c, spos);

  return 0;
}

int XStore::_create_collection(
  coll_t c,
  const SequencerPosition &spos)
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "create_collection " << fn << dendl;
  int r = ::mkdir(fn, 0755);
  if (r < 0)
    r = -errno;
  if (r == -EEXIST && replaying)
    r = 0;
  dout(10) << "create_collection " << fn << " = " << r << dendl;

  if (r < 0)
    return r;
  r = init_index(c);
  if (r < 0)
    return r;
  _set_replay_guard(c, spos);
  return 0;
}

// DEPRECATED -- remove with _split_collection_create
int XStore::_create_collection(coll_t c) 
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "create_collection " << fn << dendl;
  int r = ::mkdir(fn, 0755);
  if (r < 0)
    r = -errno;
  dout(10) << "create_collection " << fn << " = " << r << dendl;

  if (r < 0)
    return r;
  return init_index(c);
}

int XStore::_destroy_collection(coll_t c) 
{
  {
    Index from;
    int r = get_index(c, &from);
    if (r < 0)
      return r;
    assert(NULL != from.index);
    RWLock::WLocker l((from.index)->access_lock);

    r = from->prep_delete();
    if (r < 0)
      return r;
  }
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "_destroy_collection " << fn << dendl;
  int r = ::rmdir(fn);
  if (r < 0)
    r = -errno;
  dout(10) << "_destroy_collection " << fn << " = " << r << dendl;
  return r;
}


int XStore::_collection_add(coll_t c, coll_t oldcid, const ghobject_t& o,
			       const SequencerPosition& spos)
{
  dout(15) << "collection_add " << c << "/" << o << " from " << oldcid << "/" << o << dendl;
  
  int dstcmp = _check_replay_guard(c, o, spos);
  if (dstcmp < 0)
    return 0;

  // check the src name too; it might have a newer guard, and we don't
  // want to clobber it
  int srccmp = _check_replay_guard(oldcid, o, spos);
  if (srccmp < 0)
    return 0;

  // open guard on object so we don't any previous operations on the
  // new name that will modify the source inode.
  FDRef fd;
  int r = lfn_open(oldcid, o, 0, &fd);
  if (r < 0) {
    // the source collection/object does not exist. If we are replaying, we
    // should be safe, so just return 0 and move on.
    assert(replaying);
    dout(10) << "collection_add " << c << "/" << o << " from "
	     << oldcid << "/" << o << " (dne, continue replay) " << dendl;
    return 0;
  }
  if (dstcmp > 0) {      // if dstcmp == 0 the guard already says "in-progress"
    _set_replay_guard(**fd, spos, &o, true);
  }

  r = lfn_link(oldcid, c, o, o);
  if (replaying && !backend->can_checkpoint() &&
      r == -EEXIST)    // crashed between link() and set_replay_guard()
    r = 0;

  _inject_failure();

  // close guard on object so we don't do this again
  if (r == 0) {
    _close_replay_guard(**fd, spos);
  }
  lfn_close(fd);

  dout(10) << "collection_add " << c << "/" << o << " from " << oldcid << "/" << o << " = " << r << dendl;
  return r;
}

int XStore::_collection_move_rename(coll_t oldcid, const ghobject_t& oldoid,
				       coll_t c, const ghobject_t& o,
				       const SequencerPosition& spos, int osr)
{
  dout(15) << __func__ << " " << c << "/" << o << " from " << oldcid << "/" << oldoid << dendl;
  int r = 0;
  int dstcmp, srccmp;

  if (replaying) {
    /* If the destination collection doesn't exist during replay,
     * we need to delete the src object and continue on
     */
    if (!collection_exists(c))
      goto out_rm_src;
  }

  dstcmp = _check_replay_guard(c, o, spos);
  if (dstcmp < 0)
    goto out_rm_src;

  // check the src name too; it might have a newer guard, and we don't
  // want to clobber it
  srccmp = _check_replay_guard(oldcid, oldoid, spos);
  if (srccmp < 0)
    return 0;

  {
    // open guard on object so we don't any previous operations on the
    // new name that will modify the source inode.
    FDRef fd;
    r = lfn_open(oldcid, oldoid, 0, &fd);
    if (r < 0) {
      // the source collection/object does not exist. If we are replaying, we
      // should be safe, so just return 0 and move on.
      assert(replaying);
      dout(10) << __func__ << " " << c << "/" << o << " from "
	       << oldcid << "/" << oldoid << " (dne, continue replay) " << dendl;
      return 0;
    }
    if (dstcmp > 0) {      // if dstcmp == 0 the guard already says "in-progress"
      _set_replay_guard(**fd, spos, &o, true);
    }

    r = lfn_link(oldcid, c, oldoid, o);
    if (replaying && !backend->can_checkpoint() &&
	r == -EEXIST)    // crashed between link() and set_replay_guard()
      r = 0;

    _inject_failure();

    if (r == 0) {
      // the name changed; link the omap content
      if (oldoid.is_pgmeta()) {
        r = pgmeta_cache.submit_pgmeta_keys(oldoid);
      }
      if (!r) {
        r = object_map->clone(oldoid, o, &spos);
        if (r == -ENOENT)
          r = 0;
      }
    }

    _inject_failure();

    lfn_close(fd);
    fd = FDRef();

    if (r == 0)
      r = lfn_unlink(oldcid, oldoid, spos, true, osr);

    if (r == 0)
      r = lfn_open(c, o, 0, &fd);

    // close guard on object so we don't do this again
    if (r == 0)
      _close_replay_guard(**fd, spos);

    lfn_close(fd);
  }

  dout(10) << __func__ << " " << c << "/" << o << " from " << oldcid << "/" << oldoid
	   << " = " << r << dendl;
  return r;

 out_rm_src:
  // remove source
  if (_check_replay_guard(oldcid, oldoid, spos) > 0) {
    r = lfn_unlink(oldcid, oldoid, spos, true, osr);
  }

  dout(10) << __func__ << " " << c << "/" << o << " from " << oldcid << "/" << oldoid
	   << " = " << r << dendl;
  return r;
}

void XStore::_inject_failure()
{
  if (m_filestore_kill_at.read()) {
    int final = m_filestore_kill_at.dec();
    dout(5) << "_inject_failure " << (final+1) << " -> " << final << dendl;
    if (final == 0) {
      derr << "_inject_failure KILLING" << dendl;
      g_ceph_context->_log->flush();
      _exit(1);
    }
  }
}

int XStore::_omap_clear(coll_t cid, const ghobject_t &hoid,
			   const SequencerPosition &spos) {
  dout(15) << __func__ << " " << cid << "/" << hoid << dendl;
  if (hoid.is_pgmeta()) {
    pgmeta_cache.erase_pgmeta_key(hoid);
  }
  int r = object_map->clear_keys_header(hoid, &spos);
  if (r == -ENOENT) {
    Index index;
    r = get_index(cid, &index);
    if (r < 0)
      return r;
    {
      assert(NULL != index.index);
      RWLock::RLocker l((index.index)->access_lock);
      r = lfn_find(hoid, index);
      if (r < 0)
        return r;
    }
  } else if (r < 0) {
    return r;
  }
  return 0;
}

int XStore::_omap_setkeys(coll_t cid, const ghobject_t &hoid,
			     const map<string, bufferlist> &aset,
			     const SequencerPosition &spos) {
  dout(15) << __func__ << " " << cid << "/" << hoid << dendl;
  int r = 0;
  if (hoid.is_pgmeta() && !replaying) {
    if (pgmeta_cache.set_keys(hoid, aset))
      logger->inc(l_os_omap_cache_shard_flush);
  } else {
    r = object_map->set_keys(hoid, aset, &spos);
  }
  if (r == -ENOENT) {
    Index index;
    r = get_index(cid, &index);
    if (r < 0) {
      dout(20) << __func__ << " get_index got " << cpp_strerror(r) << dendl;
      return r;
    }
    {
      assert(NULL != index.index);
      RWLock::RLocker l((index.index)->access_lock);
      r = lfn_find(hoid, index);
      if (r < 0) {
        dout(20) << __func__ << " lfn_find got " << cpp_strerror(r) << dendl;
        return r;
      }
    }
  }
  dout(20) << __func__ << " " << cid << "/" << hoid << " = " << r << dendl;
  return r;
}

int XStore::_omap_rmkeys(coll_t cid, const ghobject_t &hoid,
			    const set<string> &keys,
			    const SequencerPosition &spos) {
  dout(15) << __func__ << " " << cid << "/" << hoid << dendl;
  int r = 0;
  if (hoid.is_pgmeta()) {
    pgmeta_cache.erase_keys(hoid, keys);
  }
  r = object_map->rm_keys(hoid, keys, &spos);
  if (r == -ENOENT) {
    Index index;
    r = get_index(cid, &index);
    if (r < 0)
      return r;
    {
      assert(NULL != index.index);
      RWLock::RLocker l((index.index)->access_lock);
      r = lfn_find(hoid, index);
      if (r < 0)
        return r;
    }
  } else if (r < 0) {
    return r;
  }
  return 0;
}

int XStore::_omap_rmkeyrange(coll_t cid, const ghobject_t &hoid,
				const string& first, const string& last,
				const SequencerPosition &spos) {
  dout(15) << __func__ << " " << cid << "/" << hoid << " [" << first << "," << last << "]" << dendl;
  if (hoid.is_pgmeta())
    pgmeta_cache.submit_pgmeta_keys(hoid);
  set<string> keys;
  {
    ObjectMap::ObjectMapIterator iter = get_omap_iterator(cid, hoid);
    if (!iter)
      return -ENOENT;
    for (iter->lower_bound(first); iter->valid() && iter->key() < last;
	 iter->next()) {
      keys.insert(iter->key());
    }
  }
  return _omap_rmkeys(cid, hoid, keys, spos);
}

int XStore::_omap_setheader(coll_t cid, const ghobject_t &hoid,
			       const bufferlist &bl,
			       const SequencerPosition &spos)
{
  dout(15) << __func__ << " " << cid << "/" << hoid << dendl;
  int r = object_map->set_header(hoid, bl, &spos);
  if (r == -ENOENT) {
    Index index;
    r = get_index(cid, &index);
    if (r < 0)
      return r;
    {
      assert(NULL != index.index);
      RWLock::RLocker l((index.index)->access_lock);
      r = lfn_find(hoid, index);
      if (r < 0)
        return r;
    }
  } else if (r < 0) {
    return r;
  }
  return 0;
}

int XStore::_split_collection(coll_t cid,
				 uint32_t bits,
				 uint32_t rem,
				 coll_t dest,
				 const SequencerPosition &spos)
{
  int r;
  {
    dout(15) << __func__ << " " << cid << " bits: " << bits << dendl;
    if (!collection_exists(cid)) {
      dout(2) << __func__ << ": " << cid << " DNE" << dendl;
      assert(replaying);
      return 0;
    }
    if (!collection_exists(dest)) {
      dout(2) << __func__ << ": " << dest << " DNE" << dendl;
      assert(replaying);
      return 0;
    }

    int dstcmp = _check_replay_guard(dest, spos);
    if (dstcmp < 0)
      return 0;

    int srccmp = _check_replay_guard(cid, spos);
    if (srccmp < 0)
      return 0;

    _set_global_replay_guard(cid, spos);
    _set_replay_guard(cid, spos, true);
    _set_replay_guard(dest, spos, true);

    Index from;
    r = get_index(cid, &from);

    Index to;
    if (!r)
      r = get_index(dest, &to);

    if (!r) {
      assert(NULL != from.index);
      RWLock::WLocker l1((from.index)->access_lock);

      assert(NULL != to.index);
      RWLock::WLocker l2((to.index)->access_lock);
      
      r = from->split(rem, bits, to.index);
    }

    _close_replay_guard(cid, spos);
    _close_replay_guard(dest, spos);
  }
  if (g_conf->filestore_debug_verify_split) {
    vector<ghobject_t> objects;
    ghobject_t next;
    while (1) {
      collection_list_partial(
	cid,
	next,
	get_ideal_list_min(), get_ideal_list_max(), 0,
	&objects,
	&next);
      if (objects.empty())
	break;
      for (vector<ghobject_t>::iterator i = objects.begin();
	   i != objects.end();
	   ++i) {
	dout(20) << __func__ << ": " << *i << " still in source "
		 << cid << dendl;
	assert(!i->match(bits, rem));
      }
      objects.clear();
    }
    next = ghobject_t();
    while (1) {
      collection_list_partial(
	dest,
	next,
	get_ideal_list_min(), get_ideal_list_max(), 0,
	&objects,
	&next);
      if (objects.empty())
	break;
      for (vector<ghobject_t>::iterator i = objects.begin();
	   i != objects.end();
	   ++i) {
	dout(20) << __func__ << ": " << *i << " now in dest "
		 << *i << dendl;
	assert(i->match(bits, rem));
      }
      objects.clear();
    }
  }
  return r;
}

// DEPRECATED: remove once we are sure there won't be any such transactions
// replayed
int XStore::_split_collection_create(coll_t cid,
					uint32_t bits,
					uint32_t rem,
					coll_t dest,
					const SequencerPosition &spos)
{
  dout(15) << __func__ << " " << cid << " bits: " << bits << dendl;
  int r = _create_collection(dest);
  if (r < 0 && !(r == -EEXIST && replaying))
    return r;

  int dstcmp = _check_replay_guard(cid, spos);
  if (dstcmp < 0)
    return 0;

  int srccmp = _check_replay_guard(dest, spos);
  if (srccmp < 0)
    return 0;

  _set_replay_guard(cid, spos, true);
  _set_replay_guard(dest, spos, true);

  Index from;
  r = get_index(cid, &from);

  Index to;
  if (!r) 
    r = get_index(dest, &to);

  if (!r) {
    assert(NULL != from.index);
    RWLock::WLocker l1((from.index)->access_lock);

    assert(NULL != to.index);
    RWLock::WLocker l2((to.index)->access_lock);
 
    r = from->split(rem, bits, to.index);
  }

  _close_replay_guard(cid, spos);
  _close_replay_guard(dest, spos);
  return r;
}

int XStore::_set_alloc_hint(coll_t cid, const ghobject_t& oid,
                               uint64_t expected_object_size,
                               uint64_t expected_write_size)
{
  dout(15) << "set_alloc_hint " << cid << "/" << oid << " object_size " << expected_object_size << " write_size " << expected_write_size << dendl;

  FDRef fd;
  int ret;

  ret = lfn_open(cid, oid, false, &fd);
  if (ret < 0)
    goto out;

  {
    // TODO: a more elaborate hint calculation
    uint64_t hint = MIN(expected_write_size, m_filestore_max_alloc_hint_size);

    ret = backend->set_alloc_hint(**fd, hint);
    dout(20) << "set_alloc_hint hint " << hint << " ret " << ret << dendl;
  }

  lfn_close(fd);
out:
  dout(10) << "set_alloc_hint " << cid << "/" << oid << " object_size " << expected_object_size << " write_size " << expected_write_size << " = " << ret << dendl;
  assert(!m_filestore_fail_eio || ret != -EIO);
  return ret;
}

const char** XStore::get_tracked_conf_keys() const
{
  static const char* KEYS[] = {
    "filestore_min_sync_interval",
    "filestore_max_sync_interval",
    "filestore_queue_max_ops",
    "filestore_queue_max_bytes",
    "filestore_queue_committing_max_ops",
    "filestore_queue_committing_max_bytes",
    "filestore_commit_timeout",
    "filestore_dump_file",
    "filestore_kill_at",
    "filestore_fail_eio",
    "filestore_fadvise",
    "filestore_sloppy_crc",
    "filestore_sloppy_crc_block_size",
    "filestore_max_alloc_hint_size",
    NULL
  };
  return KEYS;
}

void XStore::handle_conf_change(const struct md_config_t *conf,
			  const std::set <std::string> &changed)
{
  if (changed.count("filestore_max_inline_xattr_size") ||
      changed.count("filestore_max_inline_xattr_size_xfs") ||
      changed.count("filestore_max_inline_xattr_size_btrfs") ||
      changed.count("filestore_max_inline_xattr_size_other") ||
      changed.count("filestore_max_inline_xattrs") ||
      changed.count("filestore_max_inline_xattrs_xfs") ||
      changed.count("filestore_max_inline_xattrs_btrfs") ||
      changed.count("filestore_max_inline_xattrs_other")) {
    Mutex::Locker l(lock);
    set_xattr_limits_via_conf();
  }
  if (changed.count("filestore_min_sync_interval") ||
      changed.count("filestore_max_sync_interval") ||
      changed.count("filestore_queue_max_ops") ||
      changed.count("filestore_queue_max_bytes") ||
      changed.count("filestore_queue_committing_max_ops") ||
      changed.count("filestore_queue_committing_max_bytes") ||
      changed.count("filestore_kill_at") ||
      changed.count("filestore_fail_eio") ||
      changed.count("filestore_sloppy_crc") ||
      changed.count("filestore_sloppy_crc_block_size") ||
      changed.count("filestore_max_alloc_hint_size") ||
      changed.count("filestore_fadvise")) {
    Mutex::Locker l(lock);
    m_filestore_min_sync_interval = conf->filestore_min_sync_interval;
    m_filestore_max_sync_interval = conf->filestore_max_sync_interval;
    m_filestore_queue_max_ops = conf->filestore_queue_max_ops;
    m_filestore_queue_max_bytes = conf->filestore_queue_max_bytes;
    m_filestore_queue_committing_max_ops = conf->filestore_queue_committing_max_ops;
    m_filestore_queue_committing_max_bytes = conf->filestore_queue_committing_max_bytes;
    m_filestore_kill_at.set(conf->filestore_kill_at);
    m_filestore_fail_eio = conf->filestore_fail_eio;
    m_filestore_fadvise = conf->filestore_fadvise;
    m_filestore_sloppy_crc = conf->filestore_sloppy_crc;
    m_filestore_sloppy_crc_block_size = conf->filestore_sloppy_crc_block_size;
    m_filestore_max_alloc_hint_size = conf->filestore_max_alloc_hint_size;
  }
  if (changed.count("filestore_commit_timeout")) {
    Mutex::Locker l(sync_entry_timeo_lock);
    m_filestore_commit_timeout = conf->filestore_commit_timeout;
  }
  if (changed.count("filestore_dump_file")) {
    if (conf->filestore_dump_file.length() &&
	conf->filestore_dump_file != "-") {
      dump_start(conf->filestore_dump_file);
    } else {
      dump_stop();
    }
  }
}

void XStore::dump_start(const std::string& file)
{
  dout(10) << "dump_start " << file << dendl;
  if (m_filestore_do_dump) {
    dump_stop();
  }
  m_filestore_dump_fmt.reset();
  m_filestore_dump_fmt.open_array_section("dump");
  m_filestore_dump.open(file.c_str());
  m_filestore_do_dump = true;
}

void XStore::dump_stop()
{
  dout(10) << "dump_stop" << dendl;
  m_filestore_do_dump = false;
  if (m_filestore_dump.is_open()) {
    m_filestore_dump_fmt.close_section();
    m_filestore_dump_fmt.flush(m_filestore_dump);
    m_filestore_dump.flush();
    m_filestore_dump.close();
  }
}

void XStore::dump_transactions(list<ObjectStore::Transaction*>& ls, uint64_t seq, OpSequencer *osr)
{
  m_filestore_dump_fmt.open_array_section("transactions");
  unsigned trans_num = 0;
  for (list<ObjectStore::Transaction*>::iterator i = ls.begin(); i != ls.end(); ++i, ++trans_num) {
    m_filestore_dump_fmt.open_object_section("transaction");
    m_filestore_dump_fmt.dump_string("osr", osr->get_name());
    m_filestore_dump_fmt.dump_unsigned("seq", seq);
    m_filestore_dump_fmt.dump_unsigned("trans_num", trans_num);
    (*i)->dump(&m_filestore_dump_fmt);
    m_filestore_dump_fmt.close_section();
  }
  m_filestore_dump_fmt.close_section();
  m_filestore_dump_fmt.flush(m_filestore_dump);
  m_filestore_dump.flush();
}

void XStore::set_xattr_limits_via_conf()
{
  uint32_t fs_xattr_size;
  uint32_t fs_xattrs;

  switch (m_fs_type) {
#if defined(__linux__)
  case XFS_SUPER_MAGIC:
    fs_xattr_size = g_conf->filestore_max_inline_xattr_size_xfs;
    fs_xattrs = g_conf->filestore_max_inline_xattrs_xfs;
    break;
  case BTRFS_SUPER_MAGIC:
    fs_xattr_size = g_conf->filestore_max_inline_xattr_size_btrfs;
    fs_xattrs = g_conf->filestore_max_inline_xattrs_btrfs;
    break;
#endif
  default:
    fs_xattr_size = g_conf->filestore_max_inline_xattr_size_other;
    fs_xattrs = g_conf->filestore_max_inline_xattrs_other;
    break;
  }

  // Use override value if set
  if (g_conf->filestore_max_inline_xattr_size)
    m_filestore_max_inline_xattr_size = g_conf->filestore_max_inline_xattr_size;
  else
    m_filestore_max_inline_xattr_size = fs_xattr_size;

  // Use override value if set
  if (g_conf->filestore_max_inline_xattrs)
    m_filestore_max_inline_xattrs = g_conf->filestore_max_inline_xattrs;
  else
    m_filestore_max_inline_xattrs = fs_xattrs;
}

