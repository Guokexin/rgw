// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>

#include "common/ceph_json.h"
#include "common/utf8.h"

#include "common/errno.h"
#include "common/Formatter.h"
#include "common/Throttle.h"
#include "common/Finisher.h"

#include "rgw_rados.h"
#include "rgw_cache.h"
#include "rgw_acl.h"
#include "rgw_acl_s3.h" /* for dumping s3policy in debug log */
#include "rgw_metadata.h"
#include "rgw_bucket.h"

#include "cls/rgw/cls_rgw_ops.h"
#include "cls/rgw/cls_rgw_types.h"
#include "cls/rgw/cls_rgw_client.h"
#include "cls/refcount/cls_refcount_client.h"
#include "cls/version/cls_version_client.h"
#include "cls/log/cls_log_client.h"
#include "cls/statelog/cls_statelog_client.h"
#include "cls/lock/cls_lock_client.h"
#include "cls/user/cls_user_client.h"

#include "rgw_tools.h"

#include "common/Clock.h"

#include "include/rados/librados.hpp"
using namespace librados;
#if 0
#include "include/radosstriper/libradosstriper.hpp"
using namespace libradosstriper;
#endif
#include <string>
#include <iostream>
#include <vector>
#include <list>
#include <map>
#include <set>
#include "auth/Crypto.h" // get_random_bytes()

#include "rgw_log.h"

#include "rgw_gc.h"
#include "rgw_bgt.h"

#define dout_subsys ceph_subsys_rgw

#define MERGER_QUEUE_MEMBER_SIZE 100
#define WORKER_IDLE_TIMESPAN 5
#define RELOAD_SCHEDULER_INFO_TIMESPAN 60
#define QUEUE_BATCH_SIZE 2
#define SNAP_MERGER_V 60
using namespace std;


#define dout_subsys ceph_subsys_rgw




std::string rgw_unique_lock_name(const std::string &name, void *address) 
{
  return name + " (" + stringify(address) + ")";
}

std::ostream &operator<<(std::ostream &out, const RGW_BGT_WORKER_STATE &state)
{
	switch (state)
	{
		case RGW_BGT_WORKER_INACTIVE:
			out << "inactive";
			break;
			
		case RGW_BGT_WORKER_ACTIVE:
			out << "active";
			break;
			
		default:
		  out << "unknown";
		  break;
	}

	return out;
}

std::ostream &operator<<(std::ostream &out, const RGWInstObjNotifyOp &op)
{
	switch (op)
	{
		case RGW_NOTIFY_OP_REGISTER:
			out << "WorkerRegister";
			break;

		case RGW_NOTIFY_OP_REGISTER_RSP:
			out << "WorkerRegisterRsp";
			break;
	
		case RGW_NOTIFY_OP_ACTIVE_CHANGELOG:
			out << "ActiveChangeLog";
			break;

		case RGW_NOTIFY_OP_ACTIVE_CHANGELOG_RSP:
			out << "ActiveChangeLogRsp";
			break;
	
		case RGW_NOTIFY_OP_DISPATCH_TASK:
			out << "DispatchTask";
			break;

		case RGW_NOTIFY_OP_DISPATCH_TASK_RSP:
			out << "DispatchTaskRsp";
			break;
	
		case RGW_NOTIFY_OP_TASK_FINISH:
			out << "TaskFinished";
			break;

		case RGW_NOTIFY_OP_TASK_FINISH_RSP:
			out << "TaskFinishedRsp";
			break;
	
		default:
			out << "Unknown (" << static_cast<uint32_t>(op) << ")";
			break;
	}

	return out;	
}

std::ostream &operator<<(std::ostream &out, const RGWBgtTaskState &state)
{
	switch (state)
	{
		case RGW_BGT_TASK_WAIT_CREATE_SFMOBJ:
			out << "wait create sfm obj";
			break;
			
		case RGW_BGT_TASK_WAIT_MERGE:
			out << "wait merge";
			break;
			
		case RGW_BGT_TASK_WAIT_UPDATE_INDEX:
			out << "wait update index of small obj";
			break;
			
		case RGW_BGT_TASK_WAIT_DEL_DATA:
			out << "wait del data of small obj";
			break;
			
		case RGW_BGT_TASK_WAIT_REPORT_FINISH:
			out << "wait report finish";
			break;
			
		case RGW_BGT_TASK_FINISH:
			out << "task finished";
			break;
			
		default:
			out << "unkonown";
			break;
	}

	return out;
}

std::ostream &operator<<(std::ostream &out, const RGWBgtTaskSFMergeState &state)
{
	switch (state)
	{
		case RGW_BGT_TASK_SFM_START:
			out << "SFM start";
			break;
			
		case RGW_BGT_TASK_SFM_INDEX_CREATED:
			out << "SFM index created";
			break;
			
		case RGW_BGT_TASK_SFM_MERGED:
			out << "SFM data merged";
			break;
			
		case RGW_BGT_TASK_SFM_DATA_FLUSHED:
			out << "SFM data flushed";
			break;
			
		case RGW_BGT_TASK_SFM_INDEX_FLUSHED:
			out << "SFM index flushed";
			break;
			
		default:
			out << "unknown";
			break;
	}

	return out;
}

std::ostream &operator<<(std::ostream &out, const RGWBgtBatchTaskState &state)
{
	switch (state)
	{
		case RGW_BGT_BATCH_TASK_WAIT_CREATE:
			out << "wait create";
			break;
			
		case RGW_BGT_BATCH_TASK_WAIT_DISPATCH:
			out << "wait dispatch";
			break;
			
		case RGW_BGT_BATCH_TASK_WAIT_RM_LOG:
			out << "wait remove log file";
			break;
			
		case RGW_BGT_BATCH_TASK_WAIT_RM_LOG_ENTRY:
			out << "wait remove log entry";
			break;
			
		case RGW_BGT_BATCH_TASK_WAIT_CLEAR_TASK_ENTRY:
			out << "wait clear task entry";
			break;
			
		case RGW_BGT_BATCH_TASK_FINISH:
			out << "batch task finish";
			break;
			
		default:
			out << "unknown";
			break;
	}

	return out;
}

std::ostream &operator<<(std::ostream &out, const RGWBgtLogTransState &state)
{
	switch (state)
	{
		case RGW_BGT_LOG_TRANS_START:
			out << "log trans start";
			break;
			
		case RGW_BGT_LOG_TRANS_WAIT_CREATE_LOG_OBJ:
			out << "wait create log obj";
			break;
			
		case RGW_BGT_LOG_TRANS_WAIT_ADD_TO_INACTIVE:
			out << "wait add active log to inactive log";
			break;
			
		case RGW_BGT_LOG_TRANS_WAIT_SET_ACTIVE:
			out << "wait set new active log";
			break;
			
		case RGW_BGT_LOG_TRANS_WAIT_NOTIFY_WORKER:
			out << "wait broadcast new active log";
			break;
			
		case RGW_BGT_LOG_TRANS_FINISH:
			out << "log trans finish";
			break;
			
		default:
			out << "unknown";
			break;
	}

	return out;
}

void RGWInstObjWatchCtx::handle_notify(uint64_t notify_id,uint64_t handle, uint64_t notifier_id,  bufferlist& bl)
{
  /*guokexin*/

   ldout(m_cct, 10) << "NOTIFY"
                   << " handle " << handle
                   << " notify_id " << notify_id
                   << " from " << notifier_id
                   << dendl;
#if 0
   bl.hexdump(cout);
   m_instobj_watcher->m_instobj->m_io_ctx.notify_ack(m_instobj_watcher->m_instobj->m_name, notify_id, handle, bl);
#endif   
  return m_instobj_watcher->handle_notify(notify_id, handle, bl);
}

void RGWInstObjWatchCtx::handle_error(uint64_t handle, int err)
{
#if 0
   ldout(m_cct, 0) << "ERROR"
                   << " handle " << handle
                   << " err " << cpp_strerror(err)
                   << dendl;
#endif     
  return m_instobj_watcher->handle_error(handle, err);
}

RGWInstanceObj::RGWInstanceObj(const std::string &pool_name, const std::string &instobj_name, 
                                     RGWRados* store, CephContext* cct) : 
                                     m_cct(cct),
                                     m_store(store),
                                     m_pool(pool_name),
                                     m_name(instobj_name)
{

}

RGWInstanceObj::~RGWInstanceObj()
{
}

int RGWInstanceObj::init()
{
  librados::Rados *rad = m_store->get_rados_handle_2();
  int r = rad->ioctx_create(m_pool.c_str(), m_io_ctx);
  if (r != 0)
  {
    ldout(m_cct, 0) << __func__ << "error opening pool " << m_pool << ": "
	                  << cpp_strerror(r) << dendl;  
    return r;
  }
#if 0  
  r = libradosstriper::RadosStriper::striper_create(m_io_ctx, &m_striper);
  if (0 != r)
  {
    ldout(m_cct, 0) << "error opening pool " << m_pool << " with striper interface: "
                    << cpp_strerror(r) << dendl;  
    return r;
  }
#endif  
  r = m_io_ctx.create(m_name, true);
  if (r != 0 && -EEXIST != r)
  {
    ldout(m_cct, 0) << "error create obj " << m_name << " in pool " << m_pool << ": "
                    << cpp_strerror(r) << dendl;
    return r;
  }
  
  if (-EEXIST == r)
  {
    ldout(m_cct, 10) << "obj " << m_name << " has existed in pool " << m_pool << ": "
                    << cpp_strerror(r) << dendl; 
    r = 0;                
  }
  
  return r;

}

int RGWInstanceObj::register_watch(uint64_t* handle, RGWInstObjWatchCtx* ctx)
{
  return m_io_ctx.watch2(m_name, handle, ctx);
}

int RGWInstanceObj::unregister_watch(uint64_t handle)
{
  return m_io_ctx.unwatch2(handle);
}


//
int RGWInstanceObj::uninit() {
  m_io_ctx.close();
  return 0;
}

int RGWInstanceObj::rm_ins_obj( ) {
  ldout(m_cct , 5) << "rm ins obj  "<< m_name << dendl;
  int ret = m_io_ctx.remove(m_name);
  return ret;
}

RGWInstanceObjWatcher::RGWInstanceObjWatcher(RGWInstanceObj* instobj, int role, RGWRados* store, CephContext* cct) : 
                                                     m_cct(cct),
                                                     m_store(store),
                                                     m_instobj(instobj), 
                                                     m_watch_ctx(this, cct),
                                                     m_watch_handle(0),
                                                     m_watch_state(0),
                                                     m_state(0),
                                                     m_role(role)
{
  // test by guokexin
  ldout(m_cct, 10) << "RGWInstanceObjWatcher m_role" << m_role <<dendl;
}

RGWInstanceObjWatcher::~RGWInstanceObjWatcher()
{
  if (NULL != m_instobj)
  {
    if (0 != m_watch_state)
    {
      librados::Rados *rad = m_store->get_rados_handle_2();
      rad->watch_flush();
      
      unregister_watch();
    } 

    delete m_instobj;
    m_instobj = NULL;
  }
}

int RGWInstanceObjWatcher::register_watch()
{
  int ret = m_instobj->register_watch(&m_watch_handle, &m_watch_ctx);
  if (ret < 0)
  {
    return ret;
  }

  m_watch_state = RGW_INSTOBJWATCH_STATE_REGISTERED;
  return 0;
}

int RGWInstanceObjWatcher::unregister_watch()
{
  int ret = m_instobj->unregister_watch(m_watch_handle);
  if (ret < 0)
  {
    return ret;
  }

  m_watch_state = RGW_INSTOBJWATCH_STATE_UNREGISTERED;

  return 0;
}

int RGWInstanceObjWatcher::get_notify_rsp(bufferlist& ack_bl, RGWInstObjNotifyRspMsg& notify_rsp)
{
  typedef std::map<std::pair<uint64_t, uint64_t>, bufferlist> responses_t;
  responses_t responses;
  try
  {
    bufferlist::iterator iter = ack_bl.begin();
    ldout(m_cct, 20) << "ack_bl size:" << ack_bl.length() << dendl;
    ::decode(responses, iter);
  }catch(const buffer::error &err)
  {
    ldout(m_cct, 0) << " error decoding responses: "
                    << err.what() << dendl;  
    return -1;                
  }

  bufferlist response;
  for (responses_t::iterator i = responses.begin(); i != responses.end(); ++i)
  {
    if (i->second.length() > 0)
    {
      response.claim(i->second);
    }
  }

  try
  {
    ldout(m_cct, 20) << "response size:" << response.length() << dendl;
    bufferlist::iterator iter = response.begin();
    ::decode(notify_rsp, iter);
  }catch(const buffer::error &err)
  {
    ldout(m_cct, 0) << " error decoding notify rsp: "
                    << err.what() << dendl;
    return -1;                
  }  
  
  return 0;
}

void RGWInstanceObjWatcher::process_payload(uint64_t notify_id, uint64_t handle, const RGWInstObjPayload &payload, int r)
{
  if (r != 0)
  {
    bufferlist reply_bl;
    acknowledge_notify(notify_id, handle, reply_bl);
  }
  else
  {
    apply_visitor(RGWInstObjHandlePayloadVisitor(this, notify_id, handle), payload);
  }
}

void RGWInstanceObjWatcher::acknowledge_notify(uint64_t notify_id, uint64_t handle, bufferlist &ack_bl)
{
  ldout(m_cct, 10) << "Notify ack to: " << m_instobj->m_name << ", ack size:" << ack_bl.length() << dendl; 
  m_instobj->m_io_ctx.notify_ack(m_instobj->m_name, notify_id, handle, ack_bl);
}

C_RGWInstObjNotifyAck::C_RGWInstObjNotifyAck(RGWInstanceObjWatcher *inst_obj_watcher_, uint64_t notify_id_, uint64_t handle_) :
                                                    inst_obj_watcher(inst_obj_watcher_), notify_id(notify_id_), handle(handle_) 
{
  CephContext *cct = inst_obj_watcher->m_cct;
  ldout(cct, 10) << " C_NotifyAck start: id=" << notify_id << ", "
                << "handle=" << handle << dendl;
}

void C_RGWInstObjNotifyAck::finish(int r) 
{
  CephContext *cct = inst_obj_watcher->m_cct;
  ldout(cct, 10) << " C_NotifyAck finish: id=" << notify_id << ", "
                << "handle=" << handle << dendl;

  inst_obj_watcher->acknowledge_notify(notify_id, handle, out);
}

void C_RGWInstObjResponseMessage::finish(int r) 
{
#if 0
  CephContext *cct = notify_ack->inst_obj_watcher->m_cct;
  ldout(cct, 10) << this << " C_ResponseMessage: r=" << r << dendl;

  ::encode(ResponseMessage(r), notify_ack->out);
  notify_ack->complete(0);
#endif  
}




void RGWInstanceObjWatcher::handle_notify(uint64_t notify_id, uint64_t handle, bufferlist &bl)
{
  RGWInstObjNotifyMsg notify_msg;
  try
  {
    bufferlist::iterator iter = bl.begin();
    ::decode(notify_msg, iter);
  }catch(const buffer::error &err)
  {
    ldout(m_cct, 0) << " error decoding rgw inst obj notification: "
		                << err.what() << dendl;  
		return;                
  }

  process_payload(notify_id, handle, notify_msg.payload, 0);
}


/* Begin added by hechuang */
void RGWInstanceObjWatcher::reinit() {
    int ret = unregister_watch();
    if (ret < 0) {
      ldout(m_cct, 0) << "ERROR: unregister_watch() returned ret=" << ret << dendl;
      return;
    }
    ret = register_watch();
    if (ret < 0) {
      ldout(m_cct, 0) << "ERROR: register_watch() returned ret=" << ret << dendl;
      return;
    }

    ldout(m_cct, 0) << "rgw inst obj reinit watcher success." << dendl;
}       
/* End added */


void RGWInstanceObjWatcher::handle_error(uint64_t handle, int err)
{
  ldout(m_cct, 0) << " rgw inst obj watch failed: " << handle << ", "
                  << cpp_strerror(err) << dendl;
  if (RGW_INSTOBJWATCH_STATE_REGISTERED == m_watch_state)
  {
        /* Bggin added by hechuang */
    m_store->schedule_context(new C_ReinitWatch(this));
    /* End added */
  
    //m_watch_state = RGW_INSTOBJWATCH_STATE_ERROR;
    //m_instobj->m_io_ctx.unwatch2(m_watch_handle);
  }
}

//====================================================================
int RGWBgtScheduler::update_task_entry(uint64_t task_id, std::string& log_name)
{
  int r;
  bufferlist bl;
  std::string task_name = RGW_BGT_BATCH_INST_PREFIX + log_name;
  uint64_t size = sizeof(RGWBgtTaskEntry);
  std::map<uint64_t ,RGWBgtTaskEntry>& batch_tasks = m_log_task_entry[log_name]; 
  bl.append((const char*)&batch_tasks[task_id], size);
  r = m_instobj->m_io_ctx.write(task_name, bl, size, task_id*size);
  return r;
}


int RGWBgtScheduler::notify_dispatch_task( RGWBgtWorker* worker, RGWBgtBatchTaskInfo& batch_task_info, RGWBgtTaskEntry* task_entry)
{

   
  ldout(m_cct ,20) << "notify_dispatch_task" << dendl;  

  int r = worker->start_process_task(this, batch_task_info, task_entry);
  if (r != 0)
  {
    ldout(m_cct ,0) << "dispatch task failed " << dendl;
  }
  else
  {
      
      std::map<uint64_t ,RGWBgtTaskEntry>& batch_tasks = m_log_task_entry[batch_task_info.log_name]; 
      batch_tasks[batch_task_info.next_task].dispatch_time = ceph_clock_now(0).sec();
      update_task_entry(batch_task_info.next_task, batch_task_info.log_name);
  }

  return r;
}





string RGWBgtScheduler::unique_change_log_id() 
{
  string s = m_store->unique_id(max_log_id.inc());
  //added by guokexin, log_name = scheduler_name + unique_id
  s = m_name + std::string("_") + s;
  return (RGW_BGT_CHANGELOG_PREFIX + s);
}

#if 0
void RGWBgtScheduler::update_active_change_log()
{
  worker_data_lock.Lock();

  std :: map < std :: string, RGWBgtWorkerInfo>::iterator iter = workers.begin();
  while (iter != workers.end())
  {
    RGWBgtWorkerInfo worker_info = iter->second;
    if (RGW_BGT_WORKER_ACTIVE == worker_info.state)
    {
    
     // notify_active_changelog(iter->first, active_change_log, m_cct->_conf->rgw_bgt_notify_timeout);
    }
    iter++;
  }
  worker_data_lock.Unlock();
}
#endif

//s3 rest writing log file
int RGWBgtScheduler::set_active_change_log(RGWBgtLogTransInfo& log_trans_info)
{
  int r;
  bufferlist bl;
  std :: map < std :: string,bufferlist > values;
  ::encode(log_trans_info.pending_active_change_log, bl);
  values[RGW_BGT_ACTIVE_CHANGE_LOG_KEY] = bl;
  r = m_instobj->m_io_ctx.omap_set(m_name, values);

  return r;
}
//
//mergering log file
int RGWBgtScheduler::set_change_logs(RGWBgtLogTransInfo& log_trans_info)
{
  int r;
  bufferlist bl;
  std :: map < std :: string,bufferlist > values;
  
  ::encode(change_logs[log_trans_info.active_change_log], bl);
  values[log_trans_info.active_change_log] = bl;
  r = m_instobj->m_io_ctx.omap_set(m_name, values);
  return r;
}

//added by guokexin ,20160509
int RGWBgtScheduler::set_batch_inst(RGWBatchInst& batch_inst) {
  int r = 0;
  bufferlist bl;
  std::map< std::string , bufferlist > values;
  
  ::encode(batch_inst,bl);
  values[batch_inst.inst_name] = bl;
  r = m_instobj->m_io_ctx.omap_set(m_name, values);
  return r;
}


//end added
int RGWBgtScheduler::set_batch_task_info(RGWBgtBatchTaskInfo& batch_task_info , std::string& task_name)
{
  int r;
  bufferlist bl;
  std :: map < std :: string,bufferlist > values;
  
  ::encode(batch_task_info, bl);
  values[RGW_BGT_BATCH_TASK_META_KEY] = bl;
  r = m_instobj->m_io_ctx.omap_set(task_name, values);
  return r;
}

int RGWBgtScheduler::get_batch_task_info(RGWBgtBatchTaskInfo& batch_task_info , std::string& task_name)
{
  int r;
  std :: set < std :: string >keys;
  std :: map < std :: string,bufferlist > vals;
  keys.insert(RGW_BGT_BATCH_TASK_META_KEY);
  r = m_instobj->m_io_ctx.omap_get_vals_by_keys(task_name, keys, &vals);
  if (0 == r)
  {
    if (0 == vals[RGW_BGT_BATCH_TASK_META_KEY].length())
    {
      batch_task_info.stage = RGW_BGT_BATCH_TASK_FINISH;
      ldout(m_cct, 10) << "task " << task_name << " end "  << dendl;
    }
    else
    {
      bufferlist::iterator iter = vals[RGW_BGT_BATCH_TASK_META_KEY].begin();
      ::decode(batch_task_info, iter);
      //ldout(m_cct, 10) << "load batch task info success:" << batch_task_info.stage << dendl;
    }
  }
  else
  {
    ldout(m_cct, 0) << "load batch task info failed:" << cpp_strerror(r) << dendl;
  }

  ldout(m_cct, 20) << batch_task_info.stage << dendl;
  return r;
}

void RGWBgtScheduler::clear_task_entry(RGWBgtBatchTaskInfo& batch_task_info , std::string& task_name)
{
  int r;

  std::map<std::string, std::map<uint64_t,RGWBgtTaskEntry> >::iterator it ;
  it = m_log_task_entry.find(task_name); 
  if(it != m_log_task_entry.end()) {

    std::map<uint64_t, RGWBgtTaskEntry>& batch_tasks = (*it).second;
    batch_tasks.erase(batch_tasks.begin(),batch_tasks.end());
    m_log_task_entry.erase(it);
  }
  
  r = m_instobj->m_io_ctx.trunc(task_name, 0);
  if (0 == r)
  {
    batch_task_info.stage = RGW_BGT_BATCH_TASK_FINISH;
    set_batch_task_info(batch_task_info,task_name);
    ldout(m_cct, 0) << "clear task entry success" << dendl;
  }
  else
  {
    ldout(m_cct, 0) << "clear task entry failed:" << cpp_strerror(r) << dendl;
  }

  //clear task
  ldout(m_cct , 5) << "clear_task_entry 1" << dendl;
  ldout(m_cct , 5) << "clear_task_entry 2" << dendl;
  std :: set < std :: string> taskkey;
  ldout(m_cct , 5) << "clear_task_entry 3" << dendl;
  taskkey.insert(task_name);
  ldout(m_cct , 5) << "clear_task_entry 4" << dendl;
  m_instobj->m_io_ctx.omap_rm_keys(m_name ,taskkey );
  ldout(m_cct , 5) << "clear_task_entry 5" << dendl;
  m_instobj->m_io_ctx.remove(task_name);
  ldout(m_cct , 5) << "clear_task_entry 6" << dendl;

}

void RGWBgtScheduler::rm_log_entry(RGWBgtBatchTaskInfo& batch_task_info , std::string& task_name)
{
  int r;
  std :: set < std :: string >keys;

  keys.insert(batch_task_info.log_name);
  r = m_instobj->m_io_ctx.omap_rm_keys(m_name, keys);
  if (0 == r)
  {
    std :: map < std :: string, RGWBgtChangeLogInfo>::iterator iter = change_logs.find(batch_task_info.log_name);
    if (iter != change_logs.end())
    {
      std :: map < uint64_t, std :: string>::iterator iter2 = order_change_logs.find(iter->second.order);
      order_change_logs.erase(iter2);
      change_logs.erase(iter);
    }
    batch_task_info.stage = RGW_BGT_BATCH_TASK_WAIT_CLEAR_TASK_ENTRY;
    set_batch_task_info(batch_task_info, task_name);
    ldout(m_cct , 5) << "Remove log entry success:" << batch_task_info.log_name << dendl;
  }
  else
  {
    ldout(m_cct , 5) << "Remove log entry failed:" << cpp_strerror(r) << dendl;
  }
}

void RGWBgtScheduler::rm_change_log(RGWBgtBatchTaskInfo& batch_task_info , std::string& task_name)
{
  int r;

  for (uint32_t s = 0; s < m_cct->_conf->rgw_bgt_change_log_max_shard; s++)
  {
    ostringstream oss;
    oss << batch_task_info.log_name << "_" << s;
    string change_log_shard = oss.str();  

    r = m_instobj->m_io_ctx.remove(change_log_shard);
    if (0 == r)
    {
      ldout(m_cct, 10) << "Remove log file success:" << change_log_shard << dendl;
    }
    else if (-ENOENT == r)
    {
      ldout(m_cct, 0) << "change log has been removed:" << change_log_shard << dendl;
    }
    else
    {
      ldout(m_cct, 0) << "Remove change log failed:" << change_log_shard << ", " << cpp_strerror(r) << dendl;
      return;
    }
  }

  batch_task_info.stage = RGW_BGT_BATCH_TASK_WAIT_RM_LOG_ENTRY;
  set_batch_task_info(batch_task_info,task_name);
}

/* modified by guokexin */
void RGWBgtScheduler::dispatch_task(RGWBgtBatchTaskInfo& batch_task_info , std::string& task_name)
{
  static uint64_t g_total_sfm = 0;
  ldout(m_cct , 5) << "dispatch_task log_name " << batch_task_info.log_name << " next_task " << batch_task_info.next_task << " task_cnt " <<batch_task_info.task_cnt <<dendl;
  
  std::map<uint64_t , RGWBgtTaskEntry>&  batch_tasks = m_log_task_entry[batch_task_info.log_name];  

  if (batch_task_info.next_task >= batch_task_info.task_cnt)
  {
    //if (1 == batch_task_switch.read())
    {
      for (uint64_t i = 0; i < batch_task_info.task_cnt; i++)
      {
        RGWBgtTaskEntry* task_entry = &batch_tasks[i];
        if (!task_entry->is_finished)
        {
          return;
        }
        else {
          //ldout(m_cct , 5) << "log_name : " << batch_task_info.log_name << " task_id " << i << "is finished " << "  cnt : " << batch_task_info.task_cnt << dendl;
        }
      }
    }
    //utime_t end_time = ceph_clock_now(m_cct);
    //uint64_t cost = (end_time.to_msec() - batch_task_info.start_time.to_msec())/1000;
    //uint32_t speed = 0;
    //if (cost > 0)
    //  speed = change_logs[batch_task_info.log_name].log_cnt / cost;
    //ldout(m_cct, 0) << "batch task for " << batch_task_info.log_name << " finished, merge speed " << speed << ", total merged object:" << g_total_sfm << dendl;
    ldout(m_cct, 0) << "enter RGW_BGT_BATCH_TASK_WAIT_RM_LOG" << dendl;
    batch_task_info.stage = RGW_BGT_BATCH_TASK_WAIT_RM_LOG;
    set_batch_task_info(batch_task_info,task_name);
    //wait_task_finish = false;
    return;
  }
  
  ldout(m_cct , 5) << "=== dispatch_task 02===" << dendl;
  //RGWBgtWorkerInfo* worker_info;
  RGWBgtWorker* worker = NULL;
  RGWBgtManager* manager = RGWBgtManager::instance();
  //worker_data_lock.Lock();
  //std :: map < std :: string, RGWBgtWorkerInfo>::iterator iter = workers.begin();
  while (batch_task_info.next_task < batch_task_info.task_cnt)
  {

    ldout(m_cct , 5) << "begin get idle thread" << dendl;
    worker = manager->get_idle_merger_instance();
    ldout(m_cct , 5) << "end get idle thread" << dendl;
    
    if(worker == NULL) {
       break;
    }

    ldout(m_cct , 5) << "end get idle thread" << dendl;
    //worker_info = &(iter->second);
    //ldout(m_cct, 10) << " worker_info->state " <<  worker->get_state() << " "
		//             <<"  work_info->idle "<< worker_info->idle 
		//             << " worker_info->role "<<worker_info->role << dendl; 
    //if ((RGW_ROLE_BGT_MERGER == (worker_info->role & RGW_ROLE_BGT_MERGER)) &&
		//    (RGW_BGT_WORKER_ACTIVE == worker_info->state) &&  
		//     worker_info->idle)
    { 
      RGWBgtTaskEntry* task_entry = &batch_tasks[batch_task_info.next_task];
      int r;
      
      ldout(m_cct, 20) << "task "<< batch_task_info.next_task << "  dispatch > " << worker->m_name << dendl;
      //r = notify_dispatch_task(iter->first, batch_task_info.next_task, batch_task_info.log_name, task_entry->start_shard, task_entry->start, task_entry->end_shard, task_entry->count, m_cct->_conf->rgw_bgt_notify_timeout);
      r = notify_dispatch_task(worker, batch_task_info, task_entry );
      if (0 == r)
      {
        g_total_sfm += task_entry->count;
        ldout(m_cct,20) << "task " << batch_task_info.next_task << ": " 
                        << batch_task_info.log_name << ", " << batch_task_info.task_cnt
                        << ", " << task_entry->start_shard
                        << ", " << task_entry->start
                        << ", " << task_entry->end_shard
                        << ", " << task_entry->count << dendl;
        //worker_info->idle = 0;
        batch_task_info.next_task++;
        r = set_batch_task_info(batch_task_info,task_name);
        while(r != 0)
        {
          ldout(m_cct, 0) << "set batch task info failed:" << cpp_strerror(r) << dendl;
          usleep(10000);
          r = set_batch_task_info(batch_task_info,task_name);
        } 
      }
      else {
         ldout(m_cct, 0) <<"dispatch failed  " << r << dendl;
      }
      ldout(m_cct, 10) << "=== dispatch_task 04===" << dendl;
    }
    //worker = manager->get_idle_merger_instance();
  }
  //worker_data_lock.Unlock();
  //wait_task_finish = true;
}





/* modified by guokexin */
#if 0
void RGWBgtScheduler::dispatch_task(RGWBgtBatchTaskInfo& batch_task_info)
{
  static uint64_t g_total_sfm = 0;
  ldout(m_cct, 10) << "=== dispatch_task 01===  next_task task_cnt"<< batch_task_info.next_task << batch_task_info.task_cnt <<dendl;
  if (batch_task_info.next_task >= batch_task_info.task_cnt)
  {
    //if (1 == batch_task_switch.read())
    {
      for (uint64_t i = 0; i < batch_task_info.task_cnt; i++)
      {
        RGWBgtTaskEntry* task_entry = &batch_tasks[i];
        if (!task_entry->is_finished)
        {
          return;
        }
      }
    }
    utime_t end_time = ceph_clock_now(m_cct);
    uint64_t cost = (end_time.to_msec() - batch_task_info.start_time.to_msec())/1000;
    uint32_t speed = 0;
    if (cost > 0)
      speed = change_logs[batch_task_info.log_name].log_cnt / cost;
    ldout(m_cct, 0) << "batch task for " << batch_task_info.log_name << " finished, merge speed " << speed << ", total merged object:" << g_total_sfm << dendl;
    ldout(m_cct, 10) << "enter RGW_BGT_BATCH_TASK_WAIT_RM_LOG" << dendl;
    batch_task_info.stage = RGW_BGT_BATCH_TASK_WAIT_RM_LOG;
    set_batch_task_info(batch_task_info);
    wait_task_finish = false;
    return;
  }
  
  ldout(m_cct, 10) << "=== dispatch_task 02===" << dendl;
  while(batch_task_info.next_task < batch_task_info.task_cnt) {
    
     RGWBgtManager* manager = RGWBgtManager::instance();
     if( NULL != manager ) {
        RGWBgtWorker* worker = manager->get_idle_merger_instance();
        if( NULL != worker  ) {
            //recive a new task
            
            RGWBgtTaskEntry* task_entry = &batch_tasks[batch_task_info.next_task];
            int ret = worker->start_process_task(this, batch_task_info,task_entry); 
            if( 0 == ret ) {
              g_total_sfm += task_entry->count;
              ldout(m_cct, 0) << "task " << batch_task_info.next_task << ": " 
                        << batch_task_info.log_name << ", " << batch_task_info.task_cnt
                        << ", " << task_entry->start_shard
                        << ", " << task_entry->start
                        << ", " << task_entry->end_shard
                        << ", " << task_entry->count << dendl;
              batch_task_info.next_task++;
              ret = set_batch_task_info(batch_task_info);
              
              while(0 != ret ) {
                ldout(m_cct , 5) << "set batch task info failed" << cpp_strerror(ret) << dendl;
                usleep(10000);
                ret = set_batch_task_info(batch_task_info);
              }
            }
        }
     }
  }
  
  wait_task_finish = true;
}
#endif


#if 0
void RGWBgtScheduler::mk_task_entry_from_buf(bufferlist& bl, uint64_t rs_cnt, 
                                                   uint32_t& next_start_shard, uint64_t& next_start, uint32_t& next_log_cnt,
                                                   uint32_t cur_shard, uint64_t& log_index, uint64_t& merge_size, 
                                                   uint64_t& task_id, bool is_last_step)
{
  RGWChangeLogEntry log_entry;
  RGWBgtTaskEntry task_entry;
  uint32_t log_size;
  
  bufferlist::iterator iter;
  
  for (uint64_t j = 0; j < rs_cnt; j++)
  {
    bufferlist bl_size, bl_log;
    bl.splice(0, sizeof(uint16_t), &bl_size);
    log_size = *((uint16_t*)bl_size.c_str());
    bl.splice(0, log_size, &bl_log);
    iter = bl_log.begin();
    ::decode(log_entry, iter);
    
    merge_size += log_entry.size;
    log_index++;
    next_log_cnt++;
  
    if (merge_size >= (m_cct->_conf->rgw_bgt_merged_obj_size<<20))
    //if (merge_size >= 10*(m_cct->_conf->rgw_bgt_merged_obj_size<<10))
    {
      task_entry.start_shard = next_start_shard;
      task_entry.start = next_start;
      task_entry.count = next_log_cnt;
      task_entry.end_shard = cur_shard;
  
      batch_tasks[task_id] = task_entry;
      task_id++;
      merge_size= 0;
      next_log_cnt = 0;

      if (!is_last_step || j != rs_cnt-1)
      {
        next_start_shard = cur_shard;
        next_start = log_index;  
      }
      else
      {
        next_start_shard = cur_shard+1;
        next_start = 0;  
      }
    }
  }
}

#endif 
void RGWBgtScheduler::set_task_entries(RGWBgtBatchTaskInfo& batch_task_info, std::string& task_name)
{
  int r;
  std::map<uint64_t , RGWBgtTaskEntry>& batch_tasks = m_log_task_entry[batch_task_info.log_name]; 
  uint64_t task_cnt = batch_tasks.size();
  bufferlist bl_entries;
  std :: map < uint64_t, RGWBgtTaskEntry>::iterator iter = batch_tasks.begin();

  while (iter != batch_tasks.end())
  {
    bl_entries.append((char*)&iter->second, sizeof(RGWBgtTaskEntry));
    iter++;
  }

  librados::ObjectWriteOperation writeOp;
  bufferlist bl;
  std :: map < std :: string,bufferlist > values;

  batch_task_info.task_cnt = task_cnt;
  batch_task_info.next_task = 0;
  batch_task_info.stage = RGW_BGT_BATCH_TASK_WAIT_DISPATCH;
  batch_task_info.start_time = ceph_clock_now(m_cct);
  ::encode(batch_task_info, bl);
  values[RGW_BGT_BATCH_TASK_META_KEY] = bl;

  writeOp.omap_set(values);
  writeOp.write_full(bl_entries);
  
  r = m_instobj->m_io_ctx.operate(task_name, &writeOp);
  if (r != 0)
  {
    ldout(m_cct, 0) << "m_io_ctx.operate:" << cpp_strerror(r) << dendl;
  }
  else
  {
    ldout(m_cct, 10) << "set batch task entries success:" << task_cnt << dendl;
  }
}

#if 0
void RGWBgtScheduler::mk_log_index_from_buf
(
  char* log_buf, uint32_t buf_size, uint32_t& next_off,
  bufferlist& bl_log_index, uint32_t& log_index_cnt, int64_t& log_entry_size 
)
{
  RGWChangeLogIndex log_index;
  char* cur_ptr = log_buf;
  char* buf_end = cur_ptr+buf_size;
  
  do
  {
    uint16_t log_size = *((uint16_t*)cur_ptr);
  
    if (((uint64_t)(buf_end-cur_ptr)) < (log_size+sizeof(uint16_t)))
    {
      break;
    }
    
    log_index.off = next_off + sizeof(uint16_t);
    log_index.size = log_size;
    
    bl_log_index.append((char*)&log_index, sizeof(RGWChangeLogIndex));
    log_index_cnt++;
  
    cur_ptr += (sizeof(uint16_t) + log_size);
    next_off = log_index.off + log_index.size;
    log_entry_size -= (sizeof(uint16_t) + log_index.size);
  }while(((uint64_t)(buf_end-cur_ptr)) >= sizeof(uint16_t));

}
#endif
void RGWBgtScheduler::mk_log_index_from_buf
(
  bufferlist& log_buf_bl, uint32_t& next_off,
  bufferlist& bl_log_index, uint32_t& log_index_cnt, int64_t& log_entry_size 
)
{
  RGWChangeLogIndex log_index;

  RGWChangeLogEntry log_entry;
  bufferlist::iterator iter;
  uint16_t log_size;
  do
  {
    bufferlist bl_size, bl_log;
    
    log_buf_bl.splice(0, sizeof(uint16_t), &bl_size);
    log_size = *((uint16_t*)bl_size.c_str());
    if (log_buf_bl.length() < log_size)
    {
      break;
    }

    log_buf_bl.splice(0, log_size, &bl_log);
    iter = bl_log.begin();
    ::decode(log_entry, iter);
        
    log_index.off = next_off + sizeof(uint16_t);
    log_index.size = log_size;
    log_index.obj_size = log_entry.size;
    
    bl_log_index.append((char*)&log_index, sizeof(RGWChangeLogIndex));
    log_index_cnt++;
  
    next_off = log_index.off + log_index.size;
    log_entry_size -= (sizeof(uint16_t) + log_index.size);
  }while(log_buf_bl.length() >= sizeof(uint16_t));
}

int RGWBgtScheduler::pre_process_change_log(string& log_name, uint32_t& log_cnt)
{
  int r;
  time_t t;
  uint64_t size;  
  bufferlist bl_hdr;

  log_cnt = 0;
  ldout(m_cct, 20) << "pre process begin:" << log_name << dendl;
  
  r = m_instobj->m_io_ctx.stat(log_name, &size, &t);
  if (0 != r)
  {
    ldout(m_cct, 0) << "stat failed:" << cpp_strerror(r) << dendl;
    return r;
  }
  
  r = m_instobj->m_io_ctx.read(log_name, bl_hdr, sizeof(RGWChangeLogHdr), 0);
  if (r < 0)
  {
    ldout(m_cct, 0) << "read log header failed:" << cpp_strerror(r) << dendl;
    return r;
  }

  RGWChangeLogHdr *log_hdr = (RGWChangeLogHdr*)bl_hdr.c_str();
  if (0 != log_hdr->log_index_off)
  {
    log_cnt = log_hdr->log_index_cnt;
    ldout(m_cct, 10) << "change log has been processed." << dendl;
    return 0;
  }

  int64_t log_entry_size = size - sizeof(RGWChangeLogHdr);
  if (0 == log_entry_size)
  {
    ldout(m_cct, 0) << "no log entry in " << log_name << dendl;
    return -5;
  }
  
  bufferlist bl_log_index;
  uint32_t next_off = sizeof(RGWChangeLogHdr);
  uint32_t log_index_cnt = 0;

  uint32_t step = 1<<22;
  while (log_entry_size > step)
  {
    bufferlist bl_log_buf;
    r = m_instobj->m_io_ctx.read(log_name, bl_log_buf, step, next_off);
    if (r < 0)
    {
      ldout(m_cct, 0) << "read log entry size failed:" << cpp_strerror(r) << dendl;
      return r;
    }

    mk_log_index_from_buf(bl_log_buf, next_off, bl_log_index, log_index_cnt, log_entry_size);
  } 

  if (log_entry_size > 0)
  {
    bufferlist bl_log_buf;
    r = m_instobj->m_io_ctx.read(log_name, bl_log_buf, log_entry_size, next_off);
    if (r < 0)
    {
      ldout(m_cct, 0) << "read log entry size failed:" << cpp_strerror(r) << dendl;
      return r;
    }

    mk_log_index_from_buf(bl_log_buf, next_off, bl_log_index, log_index_cnt, log_entry_size);
  }
  
  log_hdr->log_index_cnt = log_index_cnt;
  log_hdr->log_index_off = size;

  librados::ObjectWriteOperation writeOp;

  writeOp.write(0, bl_hdr);
  writeOp.omap_set_header(bl_log_index);

  ldout(m_cct , 0 ) << "log_name " << log_name << " bl_log_index.length " << bl_log_index.length() << dendl;
  r = m_instobj->m_io_ctx.operate(log_name, &writeOp);
  if (0 != r)
  {
    ldout(m_cct, 0) << "write log index failed:" << cpp_strerror(r) << dendl;
  }

  log_cnt = log_index_cnt;
  ldout(m_cct, 20) << "pre process end:" << log_name << ", " << log_cnt << dendl;
  return r;
}

int RGWBgtScheduler::pre_process_change_log(string& log_name)
{
  int r = 0;
  uint32_t log_cnt = 0, log_shard_cnt = 0;

  RGWBgtChangeLogInfo& log_info = change_logs[log_name];
  if (-1 != log_info.log_cnt)
  {
    ldout(m_cct, 0) << "change log has been processed." << dendl;
    return 0;
  }
  
  for (uint32_t i = 0; i < m_cct->_conf->rgw_bgt_change_log_max_shard; i++)
  {
    ostringstream oss;
    oss << log_name << "_" << i;
    string change_log_shard = oss.str(); 

    r = pre_process_change_log(change_log_shard, log_shard_cnt);
    if (0 != r && -5 != r)
    {
      return r;
    }
    log_cnt += log_shard_cnt;
  }
  
  if (0 == log_cnt)
  {
    return -5;
  }
  else
  {
    RGWBgtChangeLogInfo& log_info = change_logs[log_name];
    log_info.log_cnt = log_cnt;
    uint64_t cost = (log_info.inactive_time.to_msec() - log_info.active_time.to_msec())/1000;
    uint32_t speed = 0;
    if (cost > 0)
      speed = log_cnt/cost;
    ldout(m_cct, 0) << "write data speed " << speed << dendl;
    return 0;
  }
}

#if 0
void RGWBgtScheduler::create_task(RGWBgtBatchTaskInfo& batch_task_info)
{
  int r;
  utime_t t1;
  r = pre_process_change_log(batch_task_info.log_name);
  if (0 != r)
  {
    if (-5 == r)
    {
      batch_task_info.task_cnt = 0;
      batch_task_info.next_task = 0;
      batch_task_info.stage = RGW_BGT_BATCH_TASK_WAIT_RM_LOG;
      set_batch_task_info(batch_task_info);
    }
    return;
  }

  uint32_t next_start_shard = 0;
  uint32_t next_log_cnt = 0;
  uint64_t next_start = 0;
  uint64_t merge_size = 0;
  uint64_t task_id = 0;
  uint32_t step = 4096;  

  batch_task_info.next_task = 0;
  t1 = ceph_clock_now(0);
  for (uint32_t s = 0; s < m_cct->_conf->rgw_bgt_change_log_max_shard; s++)
  {
    ostringstream oss;
    oss << batch_task_info.log_name << "_" << s;
    string change_log_shard = oss.str();   
    bufferlist bl_log_index;
    r = m_instobj->m_io_ctx.omap_get_header(change_log_shard, &bl_log_index);
    if (0 != r)
    {
      ldout(m_cct, 0) << "get log index failed:" << cpp_strerror(r) << dendl;
      return;
    }

    uint32_t log_cnt = bl_log_index.length()/sizeof(RGWChangeLogIndex);
    RGWChangeLogIndex* log_index_meta = (RGWChangeLogIndex*)bl_log_index.c_str();
    
    uint64_t log_index = 0;
    bool is_last_step = false;
    for (uint32_t i = 0; i < log_cnt; )
    {
      uint32_t start, end;

      start = i;
      if (log_cnt-i >= step)
      {
        end = i+step-1;
        i += step;
      }
      else
      {
        end = log_cnt-1;
        i = log_cnt;
        is_last_step = true;
      }
      
      bufferlist bl;
      uint32_t read_off = log_index_meta[start].off-sizeof(uint16_t);
      uint32_t read_size = (log_index_meta[end].off+log_index_meta[end].size) - read_off;
                           
      r = m_instobj->m_io_ctx.read(change_log_shard, bl, read_size, read_off);
      if (r < 0)
      {
        ldout(m_cct, 0) << "read failed:" << cpp_strerror(r) << dendl;
        return;
      }
      mk_task_entry_from_buf(bl, end-start+1, next_start_shard, next_start, 
                             next_log_cnt, s, log_index, merge_size, task_id, is_last_step);
    }
  }
  if (merge_size)
  {
    if (task_id >= 1)
    {
      batch_tasks[task_id-1].count += next_log_cnt;
      batch_tasks[task_id-1].end_shard = m_cct->_conf->rgw_bgt_change_log_max_shard-1;
    }
    else
    {
      batch_tasks[0].start_shard = next_start_shard;
      batch_tasks[0].start = 0;
      batch_tasks[0].count = next_log_cnt;
      batch_tasks[0].end_shard = m_cct->_conf->rgw_bgt_change_log_max_shard-1;
    }
  }
  t2 = ceph_clock_now(0);
  cost = t2.to_msec()-t1.to_msec();
  ldout(m_cct, 0) << "create batch task cost " << cost << dendl;
  
  set_task_entries(batch_task_info);
  t1 = ceph_clock_now(0);
  cost = t1.to_msec()-t2.to_msec();
  ldout(m_cct, 0) << "set task entries cost " << cost << dendl;  
}
#endif
void RGWBgtScheduler::create_task(RGWBgtBatchTaskInfo& batch_task_info , std::string& task_name)
{
  //ldout(m_cct , 5) << " create_task  "<< batch_task_info.log_name <<dendl;
  int r;
  utime_t t1, t2;
  
  std::map<uint64_t , RGWBgtTaskEntry>&  batch_tasks = m_log_task_entry[batch_task_info.log_name];  
  r = pre_process_change_log(batch_task_info.log_name);
  if (0 != r)
  {
    ldout(m_cct , 5) << "pre_process_change_log, error" << dendl;
    if (-5 == r)
    {
      batch_task_info.task_cnt = 0;
      batch_task_info.next_task = 0;
      batch_task_info.stage = RGW_BGT_BATCH_TASK_WAIT_RM_LOG;
      set_batch_task_info(batch_task_info, task_name);
    }
    return;
  }

  //ldout(m_cct , 5) << "create_task ...." << dendl;
  uint32_t next_start_shard = 0;
  uint32_t next_log_cnt = 0;
  uint64_t next_start = 0;
  uint64_t merge_size = 0;
  uint64_t task_id = 0;

  batch_task_info.next_task = 0;
  t1 = ceph_clock_now(0);
  for (uint32_t s = 0; s < m_cct->_conf->rgw_bgt_change_log_max_shard; s++)
  {
    ostringstream oss;
    oss << batch_task_info.log_name << "_" << s;
    string change_log_shard = oss.str();   
    bufferlist bl_log_index;
    r = m_instobj->m_io_ctx.omap_get_header(change_log_shard, &bl_log_index);
    if (0 != r)
    {
      ldout(m_cct , 5) << "log_name : " << change_log_shard << dendl;
      ldout(m_cct , 5) << "hot_pool : " << hot_pool << dendl;
      ldout(m_cct, 0) << "get log index failed:" << cpp_strerror(r) << dendl;
      return;
    }
    
    uint32_t log_cnt = bl_log_index.length()/sizeof(RGWChangeLogIndex);
    RGWChangeLogIndex* log_index_meta = (RGWChangeLogIndex*)bl_log_index.c_str();

    //ldout(m_cct , 5) << "log_cnt " << log_cnt << " bl_log_index.length() " << bl_log_index.length() << " sizeof(RGWChangeLogIndex) " << sizeof(RGWChangeLogIndex) << dendl;
    uint64_t log_index = 0;
    RGWBgtTaskEntry task_entry;
    for (uint32_t i = 0; i < log_cnt; i++)
    {
      merge_size += log_index_meta[i].obj_size;
      log_index++;
      next_log_cnt++;
      
      if (merge_size >= (m_cct->_conf->rgw_bgt_merged_obj_size<<20))
      {
        //ldout(m_cct , 5) << "test gen a sub task" << dendl;
        task_entry.start_shard = next_start_shard;
        task_entry.start = next_start;
        task_entry.count = next_log_cnt;
        task_entry.end_shard = s;
      
        batch_tasks[task_id] = task_entry;
        task_id++;
        merge_size= 0;
        next_log_cnt = 0;
      
        if (i != log_cnt-1)
        {
          next_start_shard = s;
          next_start = log_index;  
        }
        else
        {
          next_start_shard = s+1;
          next_start = 0;  
        }
      }
    }
  }
  if (merge_size)
  {
    if (task_id >= 1)
    {
      batch_tasks[task_id-1].count += next_log_cnt;
      batch_tasks[task_id-1].end_shard = m_cct->_conf->rgw_bgt_change_log_max_shard-1;
    }
    else
    {
      batch_tasks[0].start_shard = next_start_shard;
      batch_tasks[0].start = 0;
      batch_tasks[0].count = next_log_cnt;
      batch_tasks[0].end_shard = m_cct->_conf->rgw_bgt_change_log_max_shard-1;
    }
  }
  t2 = ceph_clock_now(0);
  uint64_t cost = t2.to_msec()-t1.to_msec();
  ldout(m_cct, 0) << "create batch task cost " << cost << dendl;
  
  set_task_entries(batch_task_info,task_name);
}

void RGWBgtScheduler::check_batch_task() {

//  ldout(m_cct , 5) << "check_batch_task" << dendl;
  int r = 0;
  
  //first gen a batch_task 
  //ldout(m_cct , 5) << "check_batch_task 01" << dendl;
  RGWBgtBatchTaskInfo batch_task_info;
  int size = m_processing_task_inst.size();
  //ldout(m_cct , 5) << "size = "<< size << dendl;
  std::map < uint64_t , std::string >::iterator iter = order_change_logs.begin();
  //move forward 
  int i = 0;
  while(i < size && iter != order_change_logs.end()) {
    iter++;
    i++;
  }


  //ldout(m_cct , 5) << "check_batch_task 02" << dendl;

  if ( m_processing_task_inst.size() < QUEUE_BATCH_SIZE &&  iter != order_change_logs.end()) {
 
    //create batch inst obj
    std::string batch_inst_name = RGW_BGT_BATCH_INST_PREFIX + (*iter).second; 
    r = m_instobj->m_io_ctx.create(batch_inst_name , true);
    if( r != 0 && -EEXIST != r ) {
      ldout(m_cct , 5) << "error create obj " << batch_inst_name << dendl;
    }

    if( -EEXIST == r ) {
      ldout(m_cct , 0 ) << "obj " << batch_inst_name << "has existed in pool " << ": " << cpp_strerror(r) << dendl;
      r = 0 ;
    }

    
    ldout(m_cct , 0) << "create a new task inst obj " << batch_inst_name << dendl;
    if( r == 0 ) {
      
      RGWBatchInst inst;
      inst.log_name = iter->second;
      inst.inst_name = batch_inst_name;
      set_batch_inst(inst);
      m_processing_task_inst[inst.inst_name] = inst;
      //
    }
  }


  //process the batch_task
  std :: map < std :: string , RGWBatchInst> :: iterator it ;
  for ( it = m_processing_task_inst.begin();  it != m_processing_task_inst.end() ; ((it!=m_processing_task_inst.end())?it++:it) ) {
     
     std::string task_inst_name = (*it).first;
     std::string log_name = (*it).second.log_name; 
     ldout(m_cct , 5) << "procesing inst_name : " << task_inst_name  << dendl;
     RGWBgtBatchTaskInfo batch_task_info;
     int r = get_batch_task_info(batch_task_info , task_inst_name);
     ldout(m_cct , 5) << "loop inst ls " << log_name << dendl; 
     if( r != 0 ) {
        ldout(m_cct , 5) << "failed to run task" << dendl;
        continue;
     }

     switch ( batch_task_info.stage ) {

       case RGW_BGT_BATCH_TASK_FINISH:
       {
         //check scheduler_info 
         RGWBgtManager* pManager = RGWBgtManager::instance();     
         std::string key = RGW_BGT_SCHEDULER_INST_PREFIX + hot_pool;
         RGWSchedulerInfo info;
         pManager->get_scheduler_info(key,info);
         cold_pool = info.cold_pool;
         //
         ldout(m_cct , 0) << "RGW_BGT_BATCH_TASK_FINISH " << dendl;
         batch_task_info.log_name = log_name;
         batch_task_info.stage = RGW_BGT_BATCH_TASK_WAIT_CREATE;
         set_batch_task_info(batch_task_info, task_inst_name);          

       }
       break;

       case RGW_BGT_BATCH_TASK_WAIT_CLEAR_TASK_ENTRY:
       {
         ldout(m_cct , 0) << "RGW_BGT_BATCH_TASK_WAIT_CLEAR_TASK_ENTRY" << dendl;
         clear_task_entry(batch_task_info,task_inst_name);
         m_processing_task_inst.erase(it++);
       }
       break;

      case RGW_BGT_BATCH_TASK_WAIT_RM_LOG_ENTRY:
      {
        ldout(m_cct , 0) << "RGW_BGT_BATCH_TASK_WAIT_RM_LOG_ENTRY" << dendl;
        rm_log_entry(batch_task_info,task_inst_name);
      }
      break;

      case RGW_BGT_BATCH_TASK_WAIT_RM_LOG:
      {
        ldout(m_cct , 0) << "RGW_BGT_BATCH_TASK_WAIT_RM_LOG" << dendl;
        rm_change_log(batch_task_info,task_inst_name);
      }
      break;

      case RGW_BGT_BATCH_TASK_WAIT_DISPATCH:
      {
        ldout(m_cct , 5) << " RGW_BGT_BATCH_TASK_WAIT_DISPATCH " << dendl;
        dispatch_task(batch_task_info,task_inst_name);
      }
      break;

      case RGW_BGT_BATCH_TASK_WAIT_CREATE:
      {
        ldout(m_cct , 0) << "RGW_BGT_BATCH_TASK_WAIT_CREATE" <<dendl;
        create_task(batch_task_info,task_inst_name);
      }
      break;

      default:
      {
        assert(0);
      }
      break;
 

    } 
  } 
}


#if 0
void RGWBgtScheduler::check_batch_task()
{
  RGWBgtBatchTaskInfo batch_task_info;
  int r;
  r = get_batch_task_info(batch_task_info);
  if (0 != r)
  {
    return;
  }

  switch (batch_task_info.stage)
  {
    case RGW_BGT_BATCH_TASK_FINISH:
    {
     // ldout(m_cct, 0) << "=== test RGW_BGT_BATCH_TASK_FINISH" << dendl;
      if (0 == force_merge_all.read())
      {
        //ldout(m_cct , 5) << "RGW_BGT_BATCH_TASK_FINISH  "<< order_change_logs.size() << dendl;
        //std :: map < std :: string, RGWBgtChangeLogInfo>::iterator iter = change_logs.begin();
        std :: map < uint64_t, std :: string>::iterator iter = order_change_logs.begin();
        if (iter != order_change_logs.end())
        {
          //iter++;
          //if (iter != order_change_logs.end())
          {
            //iter--;
            batch_task_info.log_name = iter->second;
            batch_task_info.stage = RGW_BGT_BATCH_TASK_WAIT_CREATE;
            set_batch_task_info(batch_task_info);          
          }
        }
      }
      else
      {
        //std :: map < std :: string, RGWBgtChangeLogInfo>::iterator iter = change_logs.begin();
        std :: map < uint64_t, std :: string>::iterator iter = order_change_logs.begin();
        if (iter != order_change_logs.end())
        {
          batch_task_info.log_name = iter->second;
          batch_task_info.stage = RGW_BGT_BATCH_TASK_WAIT_CREATE;
          set_batch_task_info(batch_task_info);          
        }
      }
    }
    break;

    case RGW_BGT_BATCH_TASK_WAIT_CLEAR_TASK_ENTRY:
    {
      ldout(m_cct, 10) << "RGW_BGT_BATCH_TASK_WAIT_CLEAR_TASK_ENTRY" << dendl;
      clear_task_entry(batch_task_info);
    }
    break;

    case RGW_BGT_BATCH_TASK_WAIT_RM_LOG_ENTRY:
    {
      ldout(m_cct, 10) << "RGW_BGT_BATCH_TASK_WAIT_RM_LOG_ENTRY" << dendl;
      rm_log_entry(batch_task_info);
    }
    break;

    case RGW_BGT_BATCH_TASK_WAIT_RM_LOG:
    {
      ldout(m_cct, 10) << "RGW_BGT_BATCH_TASK_WAIT_RM_LOG" << dendl;
      rm_change_log(batch_task_info);
    }
    break;

    case RGW_BGT_BATCH_TASK_WAIT_DISPATCH:
    {
      ldout(m_cct, 10) << " RGW_BGT_BATCH_TASK_WAIT_DISPATCH " << dendl;
      dispatch_task(batch_task_info);
    }
    break;

    case RGW_BGT_BATCH_TASK_WAIT_CREATE:
    {
      ldout(m_cct, 10) << "RGW_BGT_BATCH_TASK_WAIT_CREATE" <<dendl;
      create_task(batch_task_info);
    }
    break;

    default:
    {
      assert(0);
    }
    break;
  }
}

#endif
int RGWBgtScheduler::get_log_trans_info(RGWBgtLogTransInfo& log_trans_info)
{
  int r;
  std :: set < std :: string >keys;
  std :: map < std :: string,bufferlist > vals;
  keys.insert(RGW_BGT_LOG_TRANS_META_KEY);
  r = m_instobj->m_io_ctx.omap_get_vals_by_keys(m_name, keys, &vals);
  if (0 == r)
  {
    if (0 == vals[RGW_BGT_LOG_TRANS_META_KEY].length())
    {
      log_trans_info.stage = RGW_BGT_LOG_TRANS_FINISH;
      log_trans_info.order = 0;
    }
    else
    {
      bufferlist::iterator iter = vals[RGW_BGT_LOG_TRANS_META_KEY].begin();
      ::decode(log_trans_info, iter);
    }
  }
  else
  {
    ldout(m_cct, 0) << "load log trans info failed:" << cpp_strerror(r) << dendl;
  }

  ldout(m_cct, 20) << log_trans_info.stage << dendl;
  return r;  
}

int RGWBgtScheduler::set_log_trans_info(RGWBgtLogTransInfo& log_trans_info)
{
  int r;
  bufferlist bl;
  std :: map < std :: string,bufferlist > values;
  
  ::encode(log_trans_info, bl);
  values[RGW_BGT_LOG_TRANS_META_KEY] = bl;
  r = m_instobj->m_io_ctx.omap_set(m_name, values);

  return r;
}

void RGWBgtScheduler::check_log_trans()
{
  static utime_t t1 = ceph_clock_now(0);
  bool need_retry_set_log_trans_info = false;
  
  RGWBgtLogTransInfo log_trans_info;
  int r;
  

  r = get_log_trans_info(log_trans_info);
  if (0 != r)
  {
    return;
  }

  switch (log_trans_info.stage)
  {
    case RGW_BGT_LOG_TRANS_START:
    {
      log_trans_info.active_change_log = active_change_log;
      log_trans_info.pending_active_change_log = unique_change_log_id();
      log_trans_info.stage = RGW_BGT_LOG_TRANS_WAIT_CREATE_LOG_OBJ;
      set_log_trans_info(log_trans_info);
    }
    break;

    case RGW_BGT_LOG_TRANS_WAIT_CREATE_LOG_OBJ:
    {
      int r = 0;
      
      for (uint32_t i = 0; i < m_cct->_conf->rgw_bgt_change_log_max_shard; i++)
      {
        ostringstream oss;
        oss << log_trans_info.pending_active_change_log << "_" << i;
        string change_log_shard = oss.str();
        bufferlist bl_hdr;
        RGWChangeLogHdr log_hdr;
        bl_hdr.append((char*)&log_hdr, sizeof(RGWChangeLogHdr));
        
        r = m_instobj->m_io_ctx.write_full(change_log_shard, bl_hdr);
        if (0 != r)
        {
          ldout(m_cct, 0) << "create new change log failed:" 
                          << log_trans_info.pending_active_change_log 
                          << "," << cpp_strerror(r) << dendl;
          break;                
        }
      }

      if (0 == r)
      {
        ldout(m_cct , 0) << "create new change log success:" << log_trans_info.pending_active_change_log << dendl;
        log_trans_info.stage = RGW_BGT_LOG_TRANS_WAIT_ADD_TO_INACTIVE;
        set_log_trans_info(log_trans_info);
      }
    }
    break;

    case RGW_BGT_LOG_TRANS_WAIT_ADD_TO_INACTIVE:
    {
      if (active_change_log.length())
      {
        RGWBgtChangeLogInfo log_info;
        int r;
        log_info.order = log_trans_info.order++;
        log_info.inactive_time = ceph_clock_now(m_cct);
        log_info.active_time = log_trans_info.active_time;
        change_logs[active_change_log] = log_info; 

        r = set_change_logs(log_trans_info);
        if (0 == r)
        {
          log_trans_info.stage = RGW_BGT_LOG_TRANS_WAIT_SET_ACTIVE;
          set_log_trans_info(log_trans_info);
        }
      }
      else
      {
        log_trans_info.stage = RGW_BGT_LOG_TRANS_WAIT_SET_ACTIVE;
        set_log_trans_info(log_trans_info);
      }
      ldout(m_cct , 0) << "add log into inactive ls " << active_change_log << dendl;
    }
    break;

    case RGW_BGT_LOG_TRANS_WAIT_SET_ACTIVE:
    {
      int r = set_active_change_log(log_trans_info);
      if (0 == r)
      {
        if (active_change_log.length())
        {

          ldout(m_cct , 0) << "refresh pending log  " << dendl;
          public_active_change_log = log_trans_info.pending_active_change_log;
          order_change_logs[change_logs[active_change_log].order] = active_change_log;
          ldout(m_cct , 0) << "refresh pending log  " << public_active_change_log << dendl;
        }
        {
          RWLock::WLocker l(active_change_log_lock);
          active_change_log = log_trans_info.pending_active_change_log;
          //public_active_change_log = active_change_log;
        }
        log_trans_info.stage = RGW_BGT_LOG_TRANS_WAIT_NOTIFY_WORKER;
        log_trans_info.active_time = ceph_clock_now(m_cct);
        set_log_trans_info(log_trans_info);
      } 
    }
    break;

    case RGW_BGT_LOG_TRANS_WAIT_NOTIFY_WORKER:
    {
      public_active_change_log = active_change_log;
      log_trans_info.stage = RGW_BGT_LOG_TRANS_FINISH;
      set_log_trans_info(log_trans_info);

    }
    break;

    case RGW_BGT_LOG_TRANS_FINISH:
    {
      utime_t t2 = ceph_clock_now(0);
      if (t2.sec() - t1.sec() >= m_cct->_conf->rgw_bgt_change_log_check_interval)
      {
        int r = 0;
        time_t t;
        uint64_t size = 0;

        ostringstream oss;
        uint64_t shard_size;
        oss << log_trans_info.pending_active_change_log << "_" << 0;
        string change_log_shard = oss.str();         
        r = m_instobj->m_io_ctx.stat(change_log_shard, &shard_size, &t);
        if (0 == r)
        {
          size = shard_size*m_cct->_conf->rgw_bgt_change_log_max_shard;
          ldout(m_cct , 0) << "change_log_shard " << change_log_shard << " size=" << size  << " shard_size "<< shard_size <<dendl;
          t1 = t2;
        }
        else
        {
          ldout(m_cct , 0) << "stat " << change_log_shard << " failed:" << cpp_strerror(r) << dendl;
        }
        
        //
        if ((0 == r) 
          && ((size > (m_cct->_conf->rgw_bgt_change_log_max_shard*sizeof(RGWChangeLogHdr)) && 1 == force_merge_all.read()) 
          || ((size/= (1<<20)) >= m_cct->_conf->rgw_bgt_change_log_max_size))) 
          //|| ((size/= (1<<10)) >= m_cct->_conf->rgw_bgt_change_log_max_size))) 
        {
          ldout(m_cct , 5) << "RGW_BGT_TRANS_START  "<< "size  " << size << dendl;
          log_trans_info.stage = RGW_BGT_LOG_TRANS_START;
          r = set_log_trans_info(log_trans_info);
          need_retry_set_log_trans_info = (0 != r);
        }
      } // clock_time
      else if (need_retry_set_log_trans_info)
      {

        log_trans_info.stage = RGW_BGT_LOG_TRANS_START;
        r = set_log_trans_info(log_trans_info);
        need_retry_set_log_trans_info = (0 != r);
      }
      else
      {
       /* 
        if (order_change_logs.size() >= 1 && wait_task_finish)
        {
          std :: map < uint64_t, std :: string>::iterator iter = order_change_logs.begin();
          if ((uint32_t)-1 == change_logs[iter->second].log_cnt)
          {
            utime_t t11 = ceph_clock_now(0); 
            
            pre_process_change_log(iter->second);
            
            utime_t t22 = ceph_clock_now(0);
            uint64_t cost = t22.to_msec()-t11.to_msec();
            ldout(m_cct, 0) << "pre process change log " << iter->second << " cost " << cost << ", log cnt:" << change_logs[iter->second].log_cnt << dendl;            
          }
        }
        */
      }
    }
    break;
    
    default:
    {
    }
    break;
  }
}

void* RGWBgtScheduler::entry()
{
  force_merge_all.set(0);
  batch_task_switch.set(1);
  while (!stopping)
  {
    uint32_t sec = m_cct->_conf->rgw_bgt_tick_interval/1000;
    uint32_t nsec = (m_cct->_conf->rgw_bgt_tick_interval % 1000)*1000*1000; 

    //ldout(m_cct , 5) << "RGWBgtScheduler::entry 1" << dendl;
    check_log_trans();

    //ldout(m_cct , 5) << "RGWBgtScheduler::entry 2" << dendl;
    if (1 == batch_task_switch.read())
    {
      check_batch_task();
    }
    
    //ldout(m_cct , 5) << "RGWBgtScheduler::entry 3" << dendl;
    lock.Lock();
    cond.WaitInterval(NULL, lock, utime_t(sec, nsec));
    lock.Unlock();
    //ldout(m_cct , 5) << "RGWBgtScheduler::entry 4" << dendl;
  }

  return NULL;
}
void RGWBgtScheduler::start()
{
  {
    Mutex::Locker l(lock);
    stopping = false;
  }
  create();
}
void RGWBgtScheduler::stop()
{
  {
    Mutex::Locker l(lock);
    stopping = true;
    cond.Signal();
  }

  join();
}

void RGWBgtScheduler::load_change_logs()
{
  int r;
  std :: map < std :: string,bufferlist > logs;
  r = m_instobj->m_io_ctx.omap_get_vals(m_name, "", RGW_BGT_CHANGELOG_PREFIX, 1000, &logs);
  if (0 == r)
  {
    std :: map < std :: string,bufferlist >::iterator ilog = logs.begin();
    while (ilog != logs.end())
    {
      bufferlist::iterator iter = ilog->second.begin();
      RGWBgtChangeLogInfo log_info;
      ::decode(log_info, iter);
      change_logs[ilog->first] = log_info;
      order_change_logs[log_info.order] = ilog->first;

      ldout(m_cct, 0) << " load order change log " <<ilog->first << ":" << log_info.order << dendl;
      ilog++;
    }
  }
}

//load batch inst:
void RGWBgtScheduler::load_batch_inst( ) {
  m_processing_task_inst.erase(m_processing_task_inst.begin(),  m_processing_task_inst.end());
  int r = 0 ;
  std::map< std::string, bufferlist > insts;
  r = m_instobj->m_io_ctx.omap_get_vals( m_name , "" , RGW_BGT_BATCH_INST_PREFIX , 1000, &insts) ;
  if( 0 == r ) {
    std::map< std::string,bufferlist>::iterator iinst = insts.begin();
    while( iinst != insts.end()) {
      bufferlist::iterator iter = iinst->second.begin();
      RGWBatchInst batch_inst;
      ::decode(batch_inst,iter);
      m_processing_task_inst.insert( std::pair<std::string , RGWBatchInst>( batch_inst.inst_name, batch_inst ));
      ldout(m_cct , 0 ) << "load batch inst , inst_name : " << batch_inst.inst_name  << dendl;
      iinst++;
    }
  }
}





void RGWBgtScheduler::load_active_change_log()
{
  int r;
  RGWBgtLogTransInfo log_trans_info;
  
  r = get_log_trans_info(log_trans_info);
  assert(0==r);

  if (RGW_BGT_LOG_TRANS_FINISH == log_trans_info.stage)
  {
    if (0 == log_trans_info.pending_active_change_log.length())
    {
      log_trans_info.stage = RGW_BGT_LOG_TRANS_START;
      log_trans_info.order = 0;
      r = set_log_trans_info(log_trans_info);
      assert(0==r);
    }
    else
    {
      active_change_log = log_trans_info.pending_active_change_log;
      public_active_change_log = active_change_log;
      ldout(m_cct, 0) << "current change log :" << active_change_log << dendl;
    }
  }
  else
  {
    active_change_log = log_trans_info.active_change_log;
    public_active_change_log = active_change_log;
    ldout(m_cct, 0) << "current change log :" << active_change_log << dendl;
  }
}


#if 0
void RGWBgtScheduler::load_workers()
{
  int r;
  std :: map < std :: string,bufferlist > wks;
  r = m_instobj->m_io_ctx.omap_get_vals(m_name, "", RGW_BGT_WORKER_INST_PREFIX, 1000, &wks);
  if (0 == r)
  {
    std :: map < std :: string,bufferlist >::iterator iwk = wks.begin();
    ldout(m_cct, 0) << "workers info: " << dendl;
    while (iwk != wks.end())
    {
      bufferlist::iterator iter = iwk->second.begin();
      RGWBgtWorkerInfo worker_info;
      ::decode(worker_info, iter);
      worker_info.state = RGW_BGT_WORKER_INACTIVE;
      worker_data_lock.Lock();
      workers[iwk->first] = worker_info;
      worker_data_lock.Unlock();
      ldout(m_cct, 0) << iwk->first << ":" << worker_info.state << dendl;
      iwk++;
    }
  }
}
#endif
void RGWBgtScheduler::load_tasks()
{


  int r;
  uint64_t size;
  time_t t;

  std::map<std::string,  RGWBatchInst>::iterator it ;
  for( it = m_processing_task_inst.begin(); it != m_processing_task_inst.end() ; it++ ) { 


    std::string task_name = (*it).first;
    std::string log_name = (*it).second.log_name;
    std::map<uint64_t , RGWBgtTaskEntry>& batch_tasks = m_log_task_entry[log_name];
    RGWBgtBatchTaskInfo batch_task_info; 

    r = get_batch_task_info(batch_task_info , task_name);
    if (0 != r)
    {
      ldout(m_cct, 0) << "get batch task info failed" << dendl;
      continue;
    }

    if (RGW_BGT_BATCH_TASK_WAIT_CREATE == batch_task_info.stage ||
        RGW_BGT_BATCH_TASK_FINISH == batch_task_info.stage)
    {
      ldout(m_cct, 0) << "no batch task info" << dendl;
      continue;
    }

    r = m_instobj->m_io_ctx.stat(task_name, &size, &t);
    if (0 != r)
    {
      ldout(m_cct, 0) << "stat batch task info size failed" << dendl;
      continue;
    }

    if (batch_task_info.task_cnt*sizeof(RGWBgtTaskEntry) != size)
    {
      ldout(m_cct, 0) << "the batch task entry is incomplete:" << size 
        << "," << batch_task_info.task_cnt << "," << sizeof(RGWBgtTaskEntry) << dendl;
      assert(0);
      continue;
    }

    RGWBgtTaskEntry* task_entries;
    bufferlist bl;
    
    r = m_instobj->m_io_ctx.read(task_name, bl, size, 0);
    if (r > 0)
    {
      ldout(m_cct, 5) << "batch task for " << batch_task_info.log_name << dendl;
      task_entries = (RGWBgtTaskEntry*)bl.c_str();
      for(uint64_t i = 0; i < batch_task_info.task_cnt; i++)
      {
        batch_tasks[i] = task_entries[i];
        ldout(m_cct, 5) << "task " << i << ":" << task_entries[i].is_finished << dendl;
      }  
    }
    else
    {
      ldout(m_cct, 0) << "read task entry failed:" << cpp_strerror(r) << dendl;
      assert(0);
    }
  }
}

int get_obj_index_meta(librados::IoCtx& io_ctx, CephContext *m_cct, string bi_oid, string oid, rgw_bucket_dir_entry& bi_entry)
{
  int r;
  std :: set < std :: string >keys;
  std :: map < std :: string,bufferlist > vals;
  keys.insert(oid);
  r = io_ctx.omap_get_vals_by_keys(bi_oid, keys, &vals);
  if (0 == r)
  {
    ldout(m_cct, 10) << "bi_entry len:" << vals[oid].length() 
      << ", " << bi_oid << ", " << oid << dendl;

    bufferlist::iterator iter = vals[oid].begin();
    ::decode(bi_entry, iter);
    ldout(m_cct, 10) << "bi_entry len:" << vals[oid].length() << dendl;
    ldout(m_cct, 10) << "==============obj index info================" << dendl; 
    ldout(m_cct, 10) << "name=" << bi_entry.key.name << dendl;
    ldout(m_cct, 10) << "epoch=" << bi_entry.ver.epoch << dendl;
    ldout(m_cct, 10) << "locator=" << bi_entry.locator << dendl;

    ldout(m_cct, 10) << "category=" << bi_entry.meta.category << dendl;
    ldout(m_cct, 10) << "size=" << bi_entry.meta.size << dendl;
    ldout(m_cct, 10) << "etag=" << bi_entry.meta.etag << dendl;
    ldout(m_cct, 10) << "owner=" << bi_entry.meta.owner << dendl;
    ldout(m_cct, 10) << "display name=" << bi_entry.meta.owner_display_name << dendl;
    ldout(m_cct, 10) << "content type=" << bi_entry.meta.content_type << dendl;
    ldout(m_cct, 10) << "accounted size=" << bi_entry.meta.accounted_size << dendl;

    ldout(m_cct, 10) << "is_merged=" << bi_entry.meta.is_merged << dendl;
    ldout(m_cct, 10) << "data_pool=" << bi_entry.meta.data_pool << dendl;
    ldout(m_cct, 10) << "data_oid=" << bi_entry.meta.data_oid << dendl;
    ldout(m_cct, 10) << "index=" << bi_entry.meta.index << dendl;
    ldout(m_cct, 10) << "data_offset=" << bi_entry.meta.data_offset << dendl;
    ldout(m_cct, 10) << "data_size=" << bi_entry.meta.data_size << dendl;
  }
  else
  {
    ldout(m_cct, 0) << __func__ << "error get object index meta " << ": "
      << cpp_strerror(r) << dendl;     
  }  

  return r;
}

void RGWBgtScheduler::dump_sf_remain(Formatter * f)
{
  int r;
  uint64_t inactive_log = 0;
  uint64_t active_log = 0;
  
  worker_data_lock.Lock();
  //std :: map < std :: string, RGWBgtChangeLogInfo>::iterator iter = change_logs.begin();
  std :: map < uint64_t, std :: string>::iterator iter = order_change_logs.begin();
  while (iter != order_change_logs.end())
  {
    bufferlist bl_hdr;
    
    ldout(m_cct, 0) << "inactive log: " << iter->second << dendl;
    pre_process_change_log(iter->second);
    
    r = m_instobj->m_io_ctx.read(iter->second, bl_hdr, sizeof(RGWChangeLogHdr), 0);
    if (r < 0)
    {
      ldout(m_cct, 0) << "read log header failed:" << cpp_strerror(r) << dendl;
      iter++;
      continue;
    }
    
    RGWChangeLogHdr *log_hdr = (RGWChangeLogHdr*)bl_hdr.c_str();
    inactive_log += log_hdr->log_index_cnt;
    iter++;
  }
  
  ldout(m_cct, 0) << "active log: " << active_change_log << dendl;

  bufferlist bl_hdr;
  r = m_instobj->m_io_ctx.read(active_change_log, bl_hdr, sizeof(RGWChangeLogHdr), 0);
  if (r > 0)
  {
    RGWChangeLogHdr *log_hdr = (RGWChangeLogHdr*)bl_hdr.c_str();
    active_log = log_hdr->log_index_cnt;
  }
  else
  {
    ldout(m_cct, 0) << "read log header failed: " << cpp_strerror(r) << dendl;
  }

  f->dump_unsigned("Total count", inactive_log+active_log);
  f->dump_unsigned("Inactive log", inactive_log);
  f->dump_unsigned("Active log", active_log);
  
  worker_data_lock.Unlock();
}

void RGWBgtScheduler::sf_force_merge(Formatter * f, string status)
{
  if (status == "on")
  {
    force_merge_all.set(1);
  }
  else if (status == "off" )
  {
    force_merge_all.set(0);
  }

  f->dump_string("force merge status", status);
}

void RGWBgtScheduler::sf_switch_batch_task(Formatter * f, string status)
{
  if (status == "on")
  {
    batch_task_switch.set(1);
  }
  else if (status == "off" )
  {
    batch_task_switch.set(0);
  }
  f->dump_string("batch task status", status);
}


RGWBgtScheduler::RGWBgtScheduler(RGWRados* _store, CephContext* _cct, std::string&  _hot_pool, std::string&  _cold_pool) :
                                       m_store(_store),
                                       m_cct(_cct),
                                       stopping(true),
                                       wait_task_finish(false),
                                       lock(rgw_unique_lock_name("RGWBgtScheduler::lock" + m_cct->_conf->name.to_str(), this), false, true, false, _cct),
                                       worker_data_lock(rgw_unique_lock_name("RGWBgtScheduler::worker_data_lock"  + m_cct->_conf->name.to_str(), this), false, true, false, _cct),
                                       active_change_log_lock(rgw_unique_lock_name("RGWBgtScheduler::active_change_log_lock" + m_cct->_conf->name.to_str(), this)), 
                                       max_log_id(0), 
                                       active_change_log(""),
                                       hot_pool(_hot_pool),
                                       cold_pool(_cold_pool),
                                       update_change_log_lock(rgw_unique_lock_name("RGWBgtWorker::update_lock" + m_cct->_conf->name.to_str(), this)),
                                       change_log_num(0)
{

  m_name = RGW_BGT_SCHEDULER_INST_PREFIX + hot_pool + "_" +m_cct->_conf->name.to_str() ;
  ldout(m_cct, 0) << "gen a new scheduler instance " << m_name << dendl;
#if 0
  string bucket("Demon");
  string oid("test2.jpg");
  string bi_oid(".dir.default.214137.1.1");

  RGWBucketInfo bucket_info;
  RGWObjectCtx obj_ctx(store);
  time_t mtime; 
  int r;

  r = store->get_bucket_info(obj_ctx, "", bucket, bucket_info, &mtime);
  if (r < 0)
  {
    ldout(m_cct, 0) << "get bucket info failed:" << cpp_strerror(r) << dendl;
    return;  
  }

  ldout(m_cct, 0) << "==============bucket info================" << dendl;
  ldout(m_cct, 0) << "name=" << bucket_info.bucket.name << dendl;
  ldout(m_cct, 0) << "data pool=" << bucket_info.bucket.data_pool << dendl;
  ldout(m_cct, 0) << "data extra pool=" << bucket_info.bucket.data_extra_pool << dendl;
  ldout(m_cct, 0) << "index pool=" << bucket_info.bucket.index_pool << dendl;
  ldout(m_cct, 0) << "marker=" << bucket_info.bucket.marker << dendl;
  ldout(m_cct, 0) << "bucket id=" << bucket_info.bucket.bucket_id << dendl;
  ldout(m_cct, 0) << "bucket instance oid=" << bucket_info.bucket.oid << dendl;
#if 0
  rgw_obj obj(bucket_info.bucket, oid);
  rgw_obj_key key;
  obj.get_index_key(&key);
{
  JSONFormatter jf(true);
  jf.open_object_section("obj info");
  obj.dump(&jf);
  jf.dump_string("key.name=", key.name);
  jf.close_section();
  jf.flush(cout);
}  
#endif
  


//#if 1  
  librados::IoCtx io_ctx;
  librados::Rados *rad = store->get_rados_handle();
  r = rad->ioctx_create(bucket_info.bucket.index_pool.c_str(), io_ctx);
  if (r < 0)
  {
    ldout(m_cct, 0) << __func__ << "error opening pool " << bucket_info.bucket.index_pool << ": "
	                  << cpp_strerror(r) << dendl;  
    return;
  }

  rgw_bucket_dir_entry bi_entry;
  r = get_obj_index_meta(io_ctx, m_cct, bi_oid, oid, bi_entry);
  if (0 == r)
  {
    bi_entry.meta.is_merged = true;
    bi_entry.meta.data_pool = "xsky_bgt";
    bi_entry.meta.data_oid = "xsky_merged";
    bi_entry.meta.data_offset = 0;
    bi_entry.meta.data_size = 8024990;

    bufferlist bl;
    std :: map < std :: string,bufferlist > vals;
    ::encode(bi_entry, bl);
    vals[oid] = bl;
    r = io_ctx.omap_set(bi_oid, vals);
    if (0 == r)
    {
      ldout(m_cct, 0) << "update object index success:" << dendl;
      rgw_bucket_dir_entry bi_entry2;
      get_obj_index_meta(io_ctx, m_cct, bi_oid, oid, bi_entry2);
    }
    else
    {
      ldout(m_cct, 0) << "update object index failed:" << cpp_strerror(r) << dendl;
    }
  }
  else
  {
    ldout(m_cct, 0) << __func__ << "error get object index meta " << bucket_info.bucket.index_pool << ": "
	                  << cpp_strerror(r) << dendl;     
  }
#endif  

   
//  int ret = init();
}

int RGWBgtScheduler::init( ) {

  int ret = 0;
  m_instobj = new RGWInstanceObj(hot_pool,m_name ,m_store, m_cct);
  if (NULL == m_instobj)
  {
    ldout(m_cct, 0) << __func__ << " create RGWInstanceObj fail: " << hot_pool << "/" << hot_pool << dendl;
    return -1;
  }
  ret = m_instobj->init();
  if (ret < 0)
  {
    ldout(m_cct, 0) << __func__ << " RGWInstanceObj init fail: " << hot_pool << "/" << hot_pool << dendl;
    delete m_instobj;
    m_instobj = NULL;
    //return ret;
  }


  if(ret == 0) {

    
    load_batch_inst();
    load_active_change_log();
    load_change_logs();
    //load_workers();
    load_tasks();
    start();
  }



  return ret;
}


RGWBgtScheduler::~RGWBgtScheduler() {

  if (NULL != m_instobj)
  {
    delete m_instobj;
    m_instobj = NULL;
  }

}

int RGWBgtScheduler::write_change_log(RGWChangeLogEntry& log_entry)
{
  int r;
  string cur_active_change_log;
  bufferlist bl, bl_entry;
  uint16_t size;
  string cur_change_log_shard;

  ::encode(log_entry, bl_entry);
  size = bl_entry.length();
  bl.append((char*)&size, sizeof(size));
  bl.claim_append(bl_entry);

  {
    //ldout(m_cct , 5) << "cur_active_change_log " << public_active_change_log << dendl;
    RWLock::RLocker l(update_change_log_lock);
    cur_active_change_log = public_active_change_log;
  }
  
  ostringstream oss;
  oss << cur_active_change_log << "_" << (change_log_num.inc() & (m_cct->_conf->rgw_bgt_change_log_max_shard-1));
  cur_change_log_shard = oss.str(); 

  
  //r = m_instobj->m_striper.append(active_change_log, bl, RGW_CHANGE_LOG_ENTRY_SIZE);
  r = m_instobj->m_io_ctx.append(cur_change_log_shard, bl, bl.length());
  if (r >= 0)
  {
    ldout(m_cct, 10) << "append change log success" << dendl;
  }
  else
  {
    ldout(m_cct, 0) << "append change log failed(" << log_entry.bucket 
                    << "/" << log_entry.bi_key << ") : " << cpp_strerror(r) << dendl;
  }
  return r;
}


int RGWBgtScheduler::process_finished_task(RGWBgtWorker* worker,uint64_t task_id , std::string& log_name) {
  

  std::map<uint64_t , RGWBgtTaskEntry>& batch_tasks = m_log_task_entry[log_name]; 
  ldout(m_cct , 0) << " TaskFinished     "<< task_id << "  log_name  " << log_name << dendl;
  bufferlist bl;
  
  RGWBgtTaskEntry *task_entry;
  task_entry = &batch_tasks[task_id];
  task_entry->finish_time = ceph_clock_now(0);
  task_entry->is_finished = true;
  
  int r = update_task_entry(task_id, log_name);
  if (0 != r)
  {
    ldout(m_cct, 0) << " update task entry failed: " << task_entry << ", "
                    << cpp_strerror(r) << dendl;    
    task_entry->is_finished = false;
    return -1;
  }

  //
  RGWBgtManager* manager = RGWBgtManager::instance();
  manager->archive_num.add(task_entry->count);

  //worker_data_lock.Lock();
  //workers[payload.worker_name].idle = 1;
  //worker_data_lock.Unlock();

//  ldout(m_cct, 5) << " task " << payload.task_id << "finished from " << payload.worker_name << ", cost total time:" << task_entry->finish_time - task_entry->dispatch_time << dendl;

  return 0;
}



//=====================================================================
//RGWBgtWoker implement
//=====================================================================
RGWBgtWorker::RGWBgtWorker(RGWRados* store, CephContext* cct, std::string& name) :
                                 m_store(store),
                                 m_cct(cct),
                                 stopping(true),
                                 is_ready_for_accept_task(false),
                                 m_name(name),
                                 lock(rgw_unique_lock_name(m_name, this), false, true, false, cct),
                                 max_sfm_id(0),
                                 active_change_log(""),
                                 m_sfm_io_ctx(NULL),
                                 merge_stage(RGW_BGT_TASK_SFM_START), 
                                 sfm_obj_data_off(0),
                                 m_archive_task(cct, store, this),
                                 idle(0),
                                 pre_idle_time(ceph_clock_now(0)),
                                 m_log_io_ctx(NULL),
                                 hot_pool(""),
                                 lock1(rgw_unique_lock_name(m_name+"1", this), false, true, false, cct)
                                  
{
  ldout(m_cct , 5) << "RGWBgtWorker " << m_name << dendl;
  
}


int RGWBgtWorker::init( ) {

  ldout(m_cct , 20) << "RGWBgtWorker::init" << dendl;
  int ret = 0;
  m_instobj = new RGWInstanceObj(m_cct->_conf->rgw_primary_pool,m_name ,m_store, m_cct);
  if (NULL == m_instobj)
  {
    ldout(m_cct, 0) << __func__ << " create RGWInstanceObj fail: " << m_cct->_conf->rgw_primary_pool << "/" << m_cct->_conf->rgw_primary_pool << dendl;
    return -1;
  }

  ret = m_instobj->init();
  if (ret < 0)
  {
    ldout(m_cct, 0) << __func__ << " RGWInstanceObj init fail: " << m_cct->_conf->rgw_primary_pool << "/" << m_cct->_conf->rgw_primary_pool << dendl;
    delete m_instobj;
    m_instobj = NULL;
    //return ret;
  }

  if( ret == 0 ) {
    ldout(m_cct , 20) << "load_task" << dendl;
    load_task( );

    ldout(m_cct , 20) << "m_archive_task.start" << dendl;
    m_archive_task.start( );
    ldout(m_cct , 20) << "start worker" << dendl;
    start( );
  }
  return ret ;
}

int RGWBgtWorker::uninit( ) {
   //=============================
   stop( );
   return 0;
}



string RGWBgtWorker::unique_sfm_oid() 
{
  string s = m_store->unique_id(max_sfm_id.inc());
  s = m_name + s;
  return (RGW_BGT_MERGEFILE_PREFIX + s);
}


#if 0 
//guokexin
int RGWBgtWorker::write_change_log(RGWChangeLogEntry& log_entry)
{
  int r;
  string cur_active_change_log;
  bufferlist bl, bl_entry;
  uint16_t size;
  string cur_change_log_shard;

  ::encode(log_entry, bl_entry);
  size = bl_entry.length();
  bl.append((char*)&size, sizeof(size));
  bl.claim_append(bl_entry);

  {
    RWLock::RLocker l(update_change_log_lock);
    cur_active_change_log = active_change_log;
  }
  
  ostringstream oss;
  oss << cur_active_change_log << "_" << (change_log_num.inc() & (m_cct->_conf->rgw_bgt_change_log_max_shard-1));
  cur_change_log_shard = oss.str(); 

  
  //r = m_instobj->m_striper.append(active_change_log, bl, RGW_CHANGE_LOG_ENTRY_SIZE);
  r = m_instobj->m_io_ctx.append(cur_change_log_shard, bl, bl.length());
  if (r >= 0)
  {
    ldout(m_cct, 10) << "append change log success" << dendl;
  }
  else
  {
    ldout(m_cct, 0) << "append change log failed(" << log_entry.bucket 
                    << "/" << log_entry.bi_key << ") : " << cpp_strerror(r) << dendl;
  }
  return r;
}

#endif 
int RGWBgtWorker::create_sfm_obj(RGWBgtTaskInfo& task_info)
{
#if 0
  if (NULL != m_sfm_striper)
  {
    delete m_sfm_striper;
    m_sfm_io_ctx = NULL;
  }
#endif  
  if (NULL != m_sfm_io_ctx)
  {
    m_sfm_io_ctx->close();
    delete m_sfm_io_ctx;
    m_sfm_io_ctx = NULL;
  }
#if 0
  m_sfm_striper = new libradosstriper::RadosStriper();
  assert(NULL != m_sfm_striper);
#endif
  m_sfm_io_ctx = new librados::IoCtx();
  assert(NULL != m_sfm_io_ctx);
  
  bufferlist bl;
  librados::Rados *rad = m_store->get_rados_handle_2();
  int r = rad->ioctx_create(task_info.dst_pool.c_str(), *m_sfm_io_ctx);
  if (r != 0)
  {
    ldout(m_cct, 0) << __func__ << "error opening pool " << task_info.dst_pool << ": "
	                  << cpp_strerror(r) << dendl;  
    goto out;
  }
#if 0  
  r = libradosstriper::RadosStriper::striper_create(*m_sfm_io_ctx, m_sfm_striper);
  if (0 != r)
  {
    ldout(m_cct, 0) << "error opening pool " << task_info.dst_pool << " with striper interface: "
                    << cpp_strerror(r) << dendl;  
    goto out;
  }
#endif  
  //r = m_sfm_striper->create(task_info.dst_file);
  ldout(m_cct, 5) << "start create dst_file" << task_info.dst_file << dendl;
  r = m_sfm_io_ctx->create(task_info.dst_file, false);
  if (r != 0)
  {
    ldout(m_cct, 0) << "create sfm obj fail:" << cpp_strerror(r) << dendl;
    goto out;
  }
  
  task_info.stage = RGW_BGT_TASK_WAIT_MERGE;
  merge_stage = RGW_BGT_TASK_SFM_START;
  r = set_task_info(task_info);
  return r;
  
out:
#if 0
  delete m_sfm_striper;
  m_sfm_striper = NULL;
#endif
  delete m_sfm_io_ctx;
  m_sfm_io_ctx = NULL;  

  return r;
}

int RGWBgtWorker::set_task_info(RGWBgtTaskInfo& task_info)
{
  int r;
  bufferlist bl;
  std :: map < std :: string,bufferlist > values;
  
  ::encode(task_info, bl);
  values[RGW_BGT_TASK_META_KEY] = bl;
  r = m_instobj->m_io_ctx.omap_set(m_instobj->m_name, values);

  usleep(10000);
  return r;
}

int RGWBgtWorker::get_task_info(RGWBgtTaskInfo& task_info)
{
  ldout(m_cct, 5) << "ENTER GET TASK INFO" << dendl;
  int r;
  std :: set < std :: string >keys;
  std :: map < std :: string,bufferlist > vals;
  keys.insert(RGW_BGT_TASK_META_KEY);
  r = m_instobj->m_io_ctx.omap_get_vals_by_keys(m_instobj->m_name, keys, &vals);
  if (0 == r)
  {
    if (0 == vals[RGW_BGT_TASK_META_KEY].length())
    {
      task_info.stage = RGW_BGT_TASK_FINISH;
      ldout(m_cct, 20) << "thers is no task info" << dendl;
    }
    else
    {
      bufferlist::iterator iter = vals[RGW_BGT_TASK_META_KEY].begin();
      ::decode(task_info, iter);
      ldout(m_cct, 20) << "load task info success:" << task_info.stage << dendl;
    }
  }
  else
  {
    ldout(m_cct, 0) << "load task info failed:" << cpp_strerror(r) << dendl;
  }

  ldout(m_cct, 5) << task_info.stage << dendl;
  return r;
}


int RGWBgtWorker::notify_task_finished( std::string& scheduler_name, uint64_t task_id ,std::string& log_name)
{
  RGWBgtScheduler* scheduler = NULL;
  RGWBgtManager* manager = RGWBgtManager::instance();
  if( manager ) {
    scheduler = manager->get_adapter_scheduler_from_name(scheduler_name);
  }

  int ret = 0;
  if ( NULL != scheduler ) {
     ret = scheduler->process_finished_task( this,task_id, log_name);
  }
  else {
    ldout(m_cct , 5) << "not found match scheduler "<< scheduler_name << dendl;
  }
  return ret;
}


void RGWBgtWorker::report_task_finished(RGWBgtTaskInfo& task_info)
{
    int r = 0;

    ldout(m_cct , 10) << "report_task_finished "<< task_info.scheduler_name << dendl;
    r = notify_task_finished(task_info.scheduler_name,task_info.task_id,task_info.log_name);
    if(0 == r) {
      task_info.stage = RGW_BGT_TASK_FINISH;
      set_task_info(task_info);
    }
}

int RGWBgtWorker::mk_sfm_index_from_change_log(bufferlist& bl, uint64_t rs_cnt, uint64_t& index_id)
{
  RGWSfmIndex* index;
  RGWBucketInfo bucket_info;
  time_t mtime; 
  int r;

  RGWChangeLogEntry log_entry;
  uint32_t log_size;
  
  bufferlist::iterator iter;

  librados::Rados *rad = m_store->get_rados_handle_2();
  
  for (uint64_t j = 0; j < rs_cnt; j++)
  {
    bufferlist bl_size, bl_log;
    bl.splice(0, sizeof(uint16_t), &bl_size);
    log_size = *((uint16_t*)bl_size.c_str());
    bl.splice(0, log_size, &bl_log);
    iter = bl_log.begin();
    ::decode(log_entry, iter);

    index = &sfm_index[index_id];

    index->bucket = log_entry.bucket;

    RGWObjectCtx obj_ctx(m_store);
    r = m_store->get_bucket_info(obj_ctx, index->bucket, bucket_info, &mtime);
    if (r < 0)
    {
      ldout(m_cct, 0) << "get bucket info failed:" << cpp_strerror(r) << dendl;
      return r;     
    }
    std :: map < string, librados::IoCtx>::iterator iter_ioctx;

    iter_ioctx = data_io_ctx.find(bucket_info.bucket.data_pool);
    if (iter_ioctx == data_io_ctx.end())
    {
      r = rad->ioctx_create(bucket_info.bucket.data_pool.c_str(), data_io_ctx[bucket_info.bucket.data_pool]);
      if (r < 0)
      {
        ldout(m_cct, 0) << __func__ << "error opening pool " << bucket_info.bucket.data_pool << ": "
    	                  << cpp_strerror(r) << dendl;  
        return r;
      }      
    }
    iter_ioctx = index_io_ctx.find(bucket_info.bucket.index_pool);
    if (iter_ioctx == index_io_ctx.end())
    {
      r = rad->ioctx_create(bucket_info.bucket.index_pool.c_str(), index_io_ctx[bucket_info.bucket.index_pool]);
      if (r < 0)
      {
        ldout(m_cct, 0) << __func__ << "error opening pool " << bucket_info.bucket.index_pool << ": "
    	                  << cpp_strerror(r) << dendl;  
        return r;
      }      
    } 
    
    index->data_io_ctx = &data_io_ctx[bucket_info.bucket.data_pool];
    index->index_io_ctx = &index_io_ctx[bucket_info.bucket.index_pool];
    index->oid = bucket_info.bucket.bucket_id + "_" + log_entry.bi_key;
    index->bi_key = log_entry.bi_key;
    index->bi_oid = log_entry.bi_oid;
    index->off = 0;
    index->size = 0;
    index->result = false;

    index_id++;    
  }

  return 0;
}

int RGWBgtWorker::create_sfm_index(RGWBgtTaskInfo& task_info)
{
  if (merge_stage >= RGW_BGT_TASK_SFM_INDEX_CREATED)
    return 0;
    
  int r;
  
  uint32_t log_leave_cnt = task_info.count;
  uint32_t step = 4096;
  uint64_t index_id = 0;
  sfm_index.erase(sfm_index.begin(), sfm_index.end()); 

  for (uint32_t s = task_info.start_shard; s <= task_info.end_shard; s++)
  {
    bufferlist bl_log_index;
    uint32_t start_index = 0;
    ostringstream oss;
    oss << task_info.log_name << "_" << s;
    string change_log_shard = oss.str();  

    //r = m_instobj->m_io_ctx.omap_get_header(change_log_shard, &bl_log_index);
    if(m_log_io_ctx != NULL) {
      r = m_log_io_ctx->omap_get_header(change_log_shard, &bl_log_index);
      if (0 != r)
      {
        ldout(m_cct , 5) << "log_name : " <<change_log_shard << dendl;
        ldout(m_cct, 0) << "get log index failed:" << cpp_strerror(r) << dendl;
        return r;
      }
    }

    RGWChangeLogIndex* log_index = (RGWChangeLogIndex*)bl_log_index.c_str();
    uint32_t log_cnt = bl_log_index.length()/sizeof(RGWChangeLogIndex);

    if (s == task_info.start_shard)
    {
      start_index = task_info.start;
      log_cnt -= start_index; 
    }
    if (s == task_info.end_shard)
    {
      log_cnt = log_leave_cnt;
    }

    for (uint32_t i = 0; i < log_cnt; )
    {
      uint32_t start, end;

      start = i+start_index;
      if (log_cnt-i >= step)
      {
        end = start+step-1;
        i += step;
      }
      else
      {
        end = start_index+log_cnt-1;
        i = log_cnt;
      }
      
      bufferlist bl;
      uint32_t read_off = log_index[start].off-sizeof(uint16_t);
      uint32_t read_size = (log_index[end].off+log_index[end].size) - read_off;

      //r = m_instobj->m_io_ctx.read(change_log_shard, bl, read_size, read_off);
      if(m_log_io_ctx != NULL ) {
        r = m_log_io_ctx->read(change_log_shard, bl, read_size, read_off);
        if (r < 0)
        {
          ldout(m_cct, 0) << "read failed:" << cpp_strerror(r) << dendl;
          return r;
        }
      }
      r = mk_sfm_index_from_change_log(bl, end-start+1, index_id);
      if (r != 0)
      {
        return r;
      }
      log_leave_cnt -= (end-start+1);
    }
  }
  
  merge_stage = RGW_BGT_TASK_SFM_INDEX_CREATED;
  return 0;
}

int RGWBgtWorker::mk_sfm_index_from_buf(RGWSfmIndexMeta* sfm_index_meta, bufferlist& bl, uint64_t rs_cnt, uint64_t& index_id)
{
  RGWSfmIndex* index;
  RGWBucketInfo bucket_info;
  time_t mtime; 
  int r;

  librados::Rados *rad = m_store->get_rados_handle_2();

  RGWSfmObjItemMeta item_meta;
  
  bufferlist::iterator iter;
  

  for (uint64_t j = 0; j < rs_cnt; j++)
  {
    bufferlist bl_item_meta;
    bl.splice(0, sfm_index_meta[index_id].meta_size, &bl_item_meta);
    iter = bl_item_meta.begin();
    ::decode(item_meta, iter);
    
    index = &sfm_index[index_id];

    RGWObjectCtx obj_ctx(m_store);
    r = m_store->get_bucket_info(obj_ctx, item_meta.bucket, bucket_info, &mtime);
    if (r < 0)
    {
      ldout(m_cct, 0) << "get bucket info failed:" << cpp_strerror(r) << dendl;
      return r;     
    }
    std :: map < string, librados::IoCtx>::iterator iter_ioctx;

    iter_ioctx = data_io_ctx.find(bucket_info.bucket.data_pool);
    if (iter_ioctx == data_io_ctx.end())
    {
      r = rad->ioctx_create(bucket_info.bucket.data_pool.c_str(), data_io_ctx[bucket_info.bucket.data_pool]);
      if (r < 0)
      {
        ldout(m_cct, 0) << __func__ << "error opening pool " << bucket_info.bucket.data_pool << ": "
    	                  << cpp_strerror(r) << dendl;  
        return r;
      }      
    }
    iter_ioctx = index_io_ctx.find(bucket_info.bucket.index_pool);
    if (iter_ioctx == index_io_ctx.end())
    {
      r = rad->ioctx_create(bucket_info.bucket.index_pool.c_str(), index_io_ctx[bucket_info.bucket.index_pool]);
      if (r < 0)
      {
        ldout(m_cct, 0) << __func__ << "error opening pool " << bucket_info.bucket.index_pool << ": "
    	                  << cpp_strerror(r) << dendl;  
        return r;
      }      
    } 
    
    index->data_io_ctx = &data_io_ctx[bucket_info.bucket.data_pool];
    index->index_io_ctx = &index_io_ctx[bucket_info.bucket.index_pool];

    index->bucket = item_meta.bucket;
    index->bi_key = item_meta.bi_key;
    index->oid = bucket_info.bucket.bucket_id + "_" + index->bi_key;
    index->bi_oid = item_meta.bi_oid;
    index->off = sfm_index_meta[index_id].off;
    index->size = sfm_index_meta[index_id].size;
    index->result = sfm_index_meta[index_id].result;

    index_id++;    
  }

  return 0;
}

int RGWBgtWorker::load_sfm_index(RGWBgtTaskInfo & task_info)
{
  int r;
  
  uint32_t step = 4096;
  uint32_t cnt;
  uint64_t index_id = 0;
  bufferlist bl_hdr;
  bufferlist bl_meta_index;
  RGWSfmIndexHeader *sfm_index_hdr;
  RGWSfmIndexMeta* sfm_index_meta;

  sfm_index.erase(sfm_index.begin(), sfm_index.end());
  
  r = m_instobj->m_io_ctx.read(m_instobj->m_name, bl_hdr, sizeof(RGWSfmIndexHeader), 0);
  if (r < 0)
  {
    ldout(m_cct, 0) << "read sfm index header failed:" << cpp_strerror(r) << dendl; 
    return r;
  }
  else
  {
    sfm_index_hdr = (RGWSfmIndexHeader*)bl_hdr.c_str();
    r = m_instobj->m_io_ctx.read(m_instobj->m_name, bl_meta_index, 
                                 sfm_index_hdr->item_cnt*sizeof(RGWSfmIndexMeta), 
                                 sizeof(RGWSfmIndexHeader));
    if (r < 0)
    {
      ldout(m_cct, 0) << "read sfm index meta failed:" << cpp_strerror(r) << dendl;     
      return r;
    }
  }

  cnt = sfm_index_hdr->item_cnt;
  sfm_index_meta = (RGWSfmIndexMeta*)bl_meta_index.c_str();
  
  for (uint64_t i = 0; i < cnt; )
  {
    uint32_t start, end;

    start = i;
    if (cnt-i >= step)
    {
      end = start+step-1;
      i += step;
    }
    else
    {
      end = cnt-1;
      i = cnt;
    }
    
    bufferlist bl;
    uint32_t read_off = sfm_index_meta[start].meta_off;
    uint32_t read_size = (sfm_index_meta[end].meta_off+sfm_index_meta[end].meta_size) - read_off;
                         
    r = m_instobj->m_io_ctx.read(m_instobj->m_name, bl, read_size, read_off);
    if (r < 0)
    {
      ldout(m_cct, 0) << "read sfm index entry failed:" << cpp_strerror(r) << dendl;
      return r;
    }

    r = mk_sfm_index_from_buf(sfm_index_meta, bl, end-start+1, index_id);
    if (r != 0)
    {
      return r;
    }
  }

  return 0;
}

void RGWBgtWorker::sfm_flush_data(RGWBgtTaskInfo& task_info)
{
  if (merge_stage >= RGW_BGT_TASK_SFM_DATA_FLUSHED)
    return;

  bufferlist bl_sfm_obj_hdr;
  uint32_t sfm_obj_hdr_size = sizeof(RGWSfmObjHeader) + task_info.count*sizeof(RGWSfmObjItemIndex);
  bl_sfm_obj_hdr.append_zero(sfm_obj_hdr_size);
  RGWSfmObjHeader *sfm_obj_hdr = (RGWSfmObjHeader*)bl_sfm_obj_hdr.c_str();
  if (NULL == sfm_obj_hdr)
  {
    ldout(m_cct, 0) << "alloc mem for sfm_obj_hdr failed:" << "sfm_flush_data" << dendl;
    return;
  }

  sfm_obj_hdr->item_cnt = task_info.count;
  
  bufferlist bl_index;
  std :: map < uint64_t, RGWSfmIndex>::iterator iter = sfm_index.begin();
  uint32_t item_meta_total_size = 0;
  while (iter != sfm_index.end())
  {
    RGWSfmObjItemMeta item_meta;
    bufferlist bl_item_meta;
    item_meta.bucket = iter->second.bucket;
    item_meta.bi_key = iter->second.bi_key;
    item_meta.bi_oid = iter->second.bi_oid;
    ::encode(item_meta, bl_item_meta);

    if (iter->second.result)
    {
      //ldout(m_cct , 5) << "off = "<< iter->second.off << " size= " << iter->second.size << " total_size =  " << sfm_obj_hdr_size + item_meta_total_size << "  meta_size " << bl_item_meta.length() << "  sfm_file = "<< task_info.dst_file<<dendl;
      sfm_obj_hdr->items[iter->first].data_off = iter->second.off;
      sfm_obj_hdr->items[iter->first].data_size = iter->second.size;
      sfm_obj_hdr->items[iter->first].meta_off = sfm_obj_hdr_size+item_meta_total_size;
      sfm_obj_hdr->items[iter->first].meta_size = bl_item_meta.length();
    }
    else
    {
      ldout(m_cct , 5) << "task failed : " << task_info.dst_file << dendl;
      sfm_obj_hdr->items[iter->first].data_off = 0;
      sfm_obj_hdr->items[iter->first].data_size = 0;
      sfm_obj_hdr->items[iter->first].meta_off = 0;
      sfm_obj_hdr->items[iter->first].meta_size = 0;
    }
    item_meta_total_size += bl_item_meta.length();
    
    bl_index.claim_append(bl_item_meta);
    iter++;
  }
  sfm_obj_hdr->data_off = sfm_obj_hdr_size+item_meta_total_size;
  sfm_obj_data_off = sfm_obj_hdr->data_off;

  librados::ObjectWriteOperation writeOp;
  writeOp.write_full(bl_sfm_obj_hdr);
  writeOp.append(bl_index);
  writeOp.append(sfm_bl);
  int r = this->m_sfm_io_ctx->operate(task_info.dst_file, &writeOp);
  if (0 == r)
  {
    uint64_t size = bl_sfm_obj_hdr.length() + bl_index.length() + sfm_bl.length();
    ldout(m_cct, 5) << "flush data success:" << task_info.dst_file << "," << size << dendl;
    merge_stage = RGW_BGT_TASK_SFM_DATA_FLUSHED;
    sfm_bl.clear();
  }
  else
  {
    ldout(m_cct, 0) << "flush data is failed:" << cpp_strerror(r) << dendl;
  }
}

void RGWBgtWorker::sfm_flush_index(RGWBgtTaskInfo& task_info)
{
  if (merge_stage < RGW_BGT_TASK_SFM_DATA_FLUSHED ||
      merge_stage >= RGW_BGT_TASK_SFM_INDEX_FLUSHED)
    return;

  int r;  
  bufferlist bl_index;
  std :: map < uint64_t, RGWSfmIndex>::iterator iter = sfm_index.begin();

  bufferlist bl_sfm_index_hdr;
  uint32_t sfm_index_hdr_size = sizeof(RGWSfmIndexHeader) + task_info.count*sizeof(RGWSfmIndexMeta);
  bl_sfm_index_hdr.append_zero(sfm_index_hdr_size);
  RGWSfmIndexHeader *sfm_index_hdr = (RGWSfmIndexHeader*)bl_sfm_index_hdr.c_str();

  sfm_index_hdr->item_cnt = task_info.count;
  
  uint32_t item_meta_total_size = 0;

  while (iter != sfm_index.end())
  {
    RGWSfmObjItemMeta item_meta;
    bufferlist bl_item_meta;
    item_meta.bucket = iter->second.bucket;
    item_meta.bi_key = iter->second.bi_key;
    item_meta.bi_oid = iter->second.bi_oid;
    ::encode(item_meta, bl_item_meta);

    sfm_index_hdr->items[iter->first].meta_off = sfm_index_hdr_size+item_meta_total_size;
    sfm_index_hdr->items[iter->first].meta_size = bl_item_meta.length();
    sfm_index_hdr->items[iter->first].off = iter->second.off;
    sfm_index_hdr->items[iter->first].size = iter->second.size;
    sfm_index_hdr->items[iter->first].result = iter->second.result;
    
    item_meta_total_size += bl_item_meta.length();
    bl_index.claim_append(bl_item_meta);
    iter++;
  }

  librados::ObjectWriteOperation writeOp;
  bufferlist bl;
  std :: map < std :: string,bufferlist > values;

  task_info.stage = RGW_BGT_TASK_WAIT_UPDATE_INDEX;
  task_info.sfm_obj_data_off = sfm_obj_data_off;
  ::encode(task_info, bl);
  values[RGW_BGT_TASK_META_KEY] = bl;

  writeOp.write_full(bl_sfm_index_hdr);
  writeOp.append(bl_index);
  writeOp.omap_set(values);

  r = m_instobj->m_io_ctx.operate(m_instobj->m_name, &writeOp);
  if (0 == r)
  {
    merge_stage = RGW_BGT_TASK_SFM_INDEX_FLUSHED;
    ldout(m_cct, 5) << "flush index success" << dendl;
  }
  else
  {
    ldout(m_cct, 0) << "flush index is failed:" << cpp_strerror(r) << dendl;
  }
}


void RGWBgtWorker::set_sfm_result(RGWBgtTaskInfo& task_info)
{
  sfm_flush_data(task_info);
  sfm_flush_index(task_info);
}

void RGWBgtWorker::process_merge_result(RGWBgtTaskInfo& task_info)
{
  ldout(m_cct , 20) << "process_merge_result" << dendl;
  std :: map < uint64_t, RGWSfmIndex>::iterator idxi;
  list<bufferlist>::iterator bli;
  RGWSfmIndex* idx;
  
  sfm_bl.clear();
  for (idxi = sfm_index.begin(); idxi != sfm_index.end(); ++idxi)
  {
    idx = &idxi->second;
    idx->off = sfm_bl.length();
    idx->size = 0;
    for (bli = idx->lbl.begin(); bli != idx->lbl.end(); ++bli)
    {
      bufferlist& bl = *bli;
      ldout(m_cct, 5) << "io len = " << bl.length() << dendl;
      idx->size += bl.length(); 
      sfm_bl.claim_append(bl);
    }
  }

  ldout(m_cct, 5) << "Total merged size is " << sfm_bl.length() << dendl;
}

void RGWBgtWorker::wait_merge(RGWBgtTaskInfo& task_info)
{
  //ldout(m_cct, 0) << "merge state:" << merge_stage << dendl;
  if (0 == create_sfm_index(task_info))
  {
    if (merge_stage < RGW_BGT_TASK_SFM_MERGED)
    {
      lock.Lock();
      //send merge commond
      /*Begin modified by guokexin*/
      ldout(m_cct, 0) << " Notify ArchiveTask Process Merge" <<dendl;
      m_archive_task.dispatch_task(1);
      /*End modified*/
      cond.Wait(lock);
      lock.Unlock();
      
      process_merge_result(task_info);
      merge_stage = RGW_BGT_TASK_SFM_MERGED;  
    }
    set_sfm_result(task_info);
    //ldout(m_cct, 0) << "merge state:" << merge_stage << dendl;
  }
}

#if 0
void RGWBgtWorker::update_index(RGWBgtTaskInfo& task_info)
{
  std :: map < uint64_t, RGWSfmIndex>::iterator idxi;
  RGWSfmIndex* idx;
  int r;
  for (idxi = sfm_index.begin(); idxi != sfm_index.end(); ++idxi)
  {
    std :: set < std :: string > keys;
    std :: map < std :: string,bufferlist > vals;
    
    idx = &idxi->second;
    if (!idx->result)
    {
      continue;
    }
    
    keys.insert(idx->bi_key);
    r = idx->index_io_ctx->omap_get_vals_by_keys(idx->bi_oid, keys, &vals);
    if (r != 0 || 0 == vals[idx->bi_key].length())
    {
      ldout(m_cct, 0) << "get obj index meta failed:" 
                      << "bucket=" << idx->bucket << "," 
                      << idx->bi_oid << "/" << idx->bi_key << dendl;
      goto err;                
    }
    else
    {
      rgw_bucket_dir_entry bi_entry;
      
      bufferlist::iterator iter = vals[idx->bi_key].begin();
      try
      {
        ::decode(bi_entry, iter);
      }catch (buffer::error& e)
      {
        ldout(m_cct, 0) << "decode bi_entry failed" << dendl;
        goto err;
      }
      
      bi_entry.meta.is_merged = true;
      bi_entry.meta.data_pool = task_info.dst_pool;
      bi_entry.meta.data_oid = task_info.dst_file;
      bi_entry.meta.index = idxi->first;
      bi_entry.meta.data_offset = idx->off+task_info.sfm_obj_data_off;
      bi_entry.meta.data_size = idx->size;
      
      bufferlist bl;
      std :: map < std :: string,bufferlist > vals;
      ::encode(bi_entry, bl);
      vals[idx->bi_key].swap(bl);
      r = idx->index_io_ctx->omap_set(idx->bi_oid, vals);
      if (r != 0)
      {
        ldout(m_cct, 0) << "update object index failed:" << cpp_strerror(r) << dendl;
        goto err;
      }
    }
  }

  ldout(m_cct, 10) << "update all object index success" << dendl;
  
  task_info.stage = RGW_BGT_TASK_WAIT_DEL_DATA;
  set_task_info(task_info);
  return;
  
err:
  return;
}
#endif

void RGWBgtWorker::update_index(RGWBgtTaskInfo& task_info)
{
  std :: map < uint64_t, RGWSfmIndex>::iterator idxi;
  RGWSfmIndex* idx;
  int r;

  std :: map < std :: string,bufferlist > vals;
  std :: map <std :: string, std :: set < std :: string > > keys;
  std :: map <std :: string, std :: set < std :: string > >::iterator bucket_keyi;
  std :: map <std :: string, std :: map < std :: string, uint64_t > > indeies;
  for (idxi = sfm_index.begin(); idxi != sfm_index.end(); ++idxi)
  {
    idx = &idxi->second;
    if (!idx->result)
    {
      continue;
    }
    keys[idx->bi_oid].insert(idx->bi_key);
    (indeies[idx->bi_oid])[idx->bi_key] = idxi->first;
  }

  for (bucket_keyi = keys.begin(); bucket_keyi != keys.end(); ++bucket_keyi)
  {
    librados::IoCtx *index_io_ctx = sfm_index[indeies[bucket_keyi->first].begin()->second].index_io_ctx;
    std :: map < std :: string, bufferlist > vals;
    r = index_io_ctx->omap_get_vals_by_keys(bucket_keyi->first, bucket_keyi->second, &vals);
    if (0 != r)
    {
      ldout(m_cct, 0) << "get obj index meta failed:" 
                      << bucket_keyi->first << dendl;
      goto err;                
    }
    else
    {
      std :: map < std :: string, bufferlist >::iterator vali;
      for (vali = vals.begin(); vali != vals.end(); ++vali)
      {
        if (vali->second.length())
        {
          rgw_bucket_dir_entry bi_entry;
          
          bufferlist::iterator iter = vali->second.begin();
          try
          {
            ::decode(bi_entry, iter);
          }catch (buffer::error& e)
          {
            ldout(m_cct, 0) << "decode bi_entry failed" << dendl;
            goto err;
          }
          
          bi_entry.meta.is_merged = true;
          bi_entry.meta.data_pool = task_info.dst_pool;
          bi_entry.meta.data_oid = task_info.dst_file;
          bi_entry.meta.index = (indeies[bucket_keyi->first])[vali->first];//idxi->first;
          bi_entry.meta.data_offset = sfm_index[bi_entry.meta.index].off+task_info.sfm_obj_data_off;
          bi_entry.meta.data_size = sfm_index[bi_entry.meta.index].size;
          
          bufferlist bl;
          ::encode(bi_entry, bl);
          vali->second.swap(bl);
        }
        else
        {
          ldout(m_cct, 0) << "get obj index meta failed:" 
                          << bucket_keyi->first << "/" << vali->first << dendl;
          goto err;                
        }
      }
      r = index_io_ctx->omap_set(bucket_keyi->first, vals);
      if (r != 0)
      {
        ldout(m_cct, 0) << "update object index failed:" << cpp_strerror(r) << dendl;
        goto err;
      }        
    }
  }

  ldout(m_cct, 10) << "update all object index success" << dendl;
  
  task_info.stage = RGW_BGT_TASK_WAIT_DEL_DATA;
  set_task_info(task_info);
  return;
  
err:
  return;
}

void RGWBgtWorker::delete_data(RGWBgtTaskInfo& task_info)
{
  ldout(m_cct, 10) << "Delete data, Wait to do!" << dendl;

  /*Begin added by guokexin*/
  lock.Lock();
  //send delete commond
  m_archive_task.dispatch_task(2);
  cond.Wait(lock);
  lock.Unlock();
  /*End added*/

  task_info.stage = RGW_BGT_TASK_WAIT_REPORT_FINISH;
  set_task_info(task_info);  
}

void RGWBgtWorker::check_task()
{
  RGWBgtTaskInfo task_info;
  int r;

  r = get_task_info(task_info);
  if (0 != r)
  {
    return;
  }

  switch (task_info.stage)
  {
    case RGW_BGT_TASK_WAIT_CREATE_SFMOBJ:
      {
        ldout(m_cct , 0) << "RGW_BGT_TASK_WAIT_CREATE_SFMOBJ" << dendl;
        create_sfm_obj(task_info);
      }
      break;

    case RGW_BGT_TASK_WAIT_MERGE:
      {
        ldout(m_cct , 0) << "RGW_BGT_TASK_WAIT_MERGE" << dendl;
        RGWBgtManager* manager = RGWBgtManager::instance();
        if(manager->b_reload_workers) {
          wait_merge(task_info);
        }
      }
      break;

    case RGW_BGT_TASK_WAIT_UPDATE_INDEX:
      {
        
        ldout(m_cct , 0) << "RGW_BGT_TASK_WAIT_UPDATE_INDEX" << dendl;
        update_index(task_info);
      }
      break;

    case RGW_BGT_TASK_WAIT_DEL_DATA:
      {
        
        ldout(m_cct , 0) << "RGW_BGT_TASK_WAIT_DEL_DATA" << dendl;
        delete_data(task_info);
      }
      break;

    case RGW_BGT_TASK_WAIT_REPORT_FINISH:
      {
        
        ldout(m_cct , 0) << "RGW_BGT_TASK_WAIT_REPORT_FINISH" << dendl;
        report_task_finished(task_info);
        pre_idle_time = ceph_clock_now(0);
      }
      break;

    case RGW_BGT_TASK_FINISH:
      {
        
        ldout(m_cct , 0) << "RGW_BGT_TASK_FINISH" << dendl;
        this->sfm_index.clear();
        this->sfm_bl.clear();
        merge_stage = RGW_BGT_TASK_SFM_START;
        set_state(1);


        ldout(m_cct , 5) << "Task finished, wait next task!" << dendl;
        lock1.Lock();
        ldout(m_cct , 5) << "begin wait" << dendl;
        is_ready_for_accept_task = false;
        //is_ready_for_accept_task = true;
        cond1.Wait(lock1);
        if( !stopping ) {
          ldout(m_cct , 5) << "wait over" << dendl;
        }
        //is_ready_for_accept_task = false;
        lock1.Unlock();
        ldout(m_cct , 5) << "begin next task" << dendl;        
       

        
      }
      break;

    default:
      {
        assert(0);
      }
      break;
  }
}

void RGWBgtWorker::load_task()
{
  int r;
  RGWBgtTaskInfo task_info;

  r = get_task_info(task_info);
  if (0 != r)
  {
    ldout(m_cct, 0) << "get task info failed" << dendl;
    assert(0);
  } 
  else
  {
    ldout(m_cct, 0) << "current task stage is: " << task_info.stage << dendl;
    if(task_info.stage != RGW_BGT_TASK_FINISH) {
       set_state(0);
    }
  }


  //create log io ctx
  if(task_info.hot_pool != "")
  {
      m_log_io_ctx = new librados::IoCtx();
      assert(NULL != m_log_io_ctx);

      bufferlist bl;
      librados::Rados *rad = m_store->get_rados_handle_2();
      int r = rad->ioctx_create(task_info.hot_pool.c_str(), *m_log_io_ctx);
      if (r < 0)
      {
        ldout(m_cct, 0) << __func__ << "error opening pool " << task_info.hot_pool << ": "
          << cpp_strerror(r) << dendl;  
        goto out;
      }
   }




  if (task_info.stage > RGW_BGT_TASK_WAIT_CREATE_SFMOBJ && 
      task_info.stage < RGW_BGT_TASK_WAIT_REPORT_FINISH)
  {
#if 0  
    m_sfm_striper = new libradosstriper::RadosStriper();
    assert(NULL != m_sfm_striper);
#endif
    {
      m_sfm_io_ctx = new librados::IoCtx();
      assert(NULL != m_sfm_io_ctx);

      bufferlist bl;
      librados::Rados *rad = m_store->get_rados_handle_2();
      int r = rad->ioctx_create(task_info.dst_pool.c_str(), *m_sfm_io_ctx);
      if (r < 0)
      {
        ldout(m_cct, 0) << __func__ << "error opening pool " << task_info.dst_pool << ": "
  	                  << cpp_strerror(r) << dendl;  
        goto out;
      }
    }

    

#if 0    
    r = libradosstriper::RadosStriper::striper_create(*m_sfm_io_ctx, m_sfm_striper);
    if (0 != r)
    {
      ldout(m_cct, 0) << "error opening pool " << task_info.dst_pool << " with striper interface: "
                      << cpp_strerror(r) << dendl;  
      goto out;
    }
#endif    
    goto done;
    
out:
    assert(0);
    return;
  }
  
done:  
  if (RGW_BGT_TASK_WAIT_UPDATE_INDEX == task_info.stage || 
      RGW_BGT_TASK_WAIT_DEL_DATA == task_info.stage)
  {
    r = load_sfm_index(task_info);
    if (0 != r)
    {
      ldout(m_cct, 0) << "load sfm index failed" << dendl;
      assert(0);
    }
  }
}

void* RGWBgtWorker::entry()
{
  while (!stopping)
  {
    check_task();
  }

  ldout(m_cct , 5) << "RGWBgtWorker::thread end" << dendl;
  return NULL;
}
void RGWBgtWorker::start()
{
  {
    Mutex::Locker l(lock);
    stopping = false;
  }
  create();

}
void RGWBgtWorker::stop()
{
  {
    Mutex::Locker l(lock1);
    stopping = true;
    cond1.Signal();
  }

  join();
}

int  RGWBgtWorker::get_state() {
  return idle;
}

void RGWBgtWorker::set_state(int _idle) {
  idle = _idle;
}

//process_task
int RGWBgtWorker::start_process_task( RGWBgtScheduler* scheduler, RGWBgtBatchTaskInfo& batch_task_info ,RGWBgtTaskEntry* _task_entry) {

  
  
  int r = 0;
  ldout(m_cct , 0) << "RGWBgtWorker::start_process_task , task_id " <<  batch_task_info.next_task  << "  task_cnt : " << batch_task_info.task_cnt << dendl; 

  RGWBgtTaskInfo task_info;
  
  r = get_task_info(task_info);
  if (0 != r)
  {
    ldout(m_cct, 0) << " get task meta info failed: " << cpp_strerror(r) << dendl;
    set_state(1);//释放线程
    return -1;
  }

  if (RGW_BGT_TASK_FINISH != task_info.stage)
  {
    set_state(0);
    ldout(m_cct, 0) << " task has not finished: " << task_info.task_id << "," << task_info.stage << dendl;
    return -2;
  }

#if 0
  lock.Lock();
  if (!is_ready_for_accept_task)
  {
    ldout(m_cct, 0) << " has not ready to accept task: " << task_info.task_id << "," << task_info.stage << dendl;
    lock.Unlock();
    return -1;
  }
  lock.Unlock();
#endif  
  //if curr hot_pool != pre hot_pool, rebuild io_ctx pool handle
  //create a new pool handle for mergr role read log_info

  m_log_io_ctx = &(scheduler->m_instobj->m_io_ctx);
  hot_pool = scheduler->hot_pool;





  task_info.dst_pool = scheduler->cold_pool;//m_cct->_conf->rgw_bgt_archive_pool;
  ldout(m_cct , 20) << "cold_pool " << scheduler->cold_pool << "scheduler_name "<< scheduler->m_name << dendl;
  task_info.dst_file = unique_sfm_oid();
  ldout(m_cct , 0 ) << "sfm file_name " << task_info.dst_file << dendl;
  task_info.task_id = batch_task_info.next_task;
  task_id = batch_task_info.next_task;
  task_info.log_name = batch_task_info.log_name;
  task_info.start_shard = _task_entry->start_shard;
  task_info.start = _task_entry->start;
  task_info.end_shard = _task_entry->end_shard;
  task_info.count = _task_entry->count;
  task_info.stage = RGW_BGT_TASK_WAIT_CREATE_SFMOBJ;
  task_info.scheduler_name = scheduler->m_name;
  task_info.hot_pool = scheduler->hot_pool;
  //ldout(m_cct, 0) << m_name << ": " << scheduler->m_name << dendl;

  r = set_task_info(task_info);
  if (r != 0)
  {
    ldout(m_cct , 5) << " set task info failed: " << task_info.task_id << cpp_strerror(r) << dendl;
    return -1;
  }

  ldout(m_cct , 5) << " set task info success: " << task_info.task_id << dendl;  

  is_ready_for_accept_task = true;
  lock1.Lock();
  cond1.Signal();
  lock1.Unlock();

  ldout(m_cct , 5) << " set task info success: " << task_info.task_id << dendl;  
  return 0;

}

//==============================================================================


RGWBgtManager RGWBgtManager::m_singleton;

RGWBgtManager* RGWBgtManager::instance( ) {

  return &m_singleton;
}


/**
 *1、create 
 * */
int RGWBgtManager::init(RGWRados* _store, CephContext* _cct) {


  int ret = 0 ;
  m_store = _store;
  m_cct = _cct;


  std::string rgw_name = m_cct->_conf->name.to_str();
  ldout(m_cct , 0) << "RGW Name : "<< rgw_name << dendl;

  schedulers_data_lock = new Mutex(rgw_unique_lock_name(string("RGWBgtManager::schedulers_data_lock")+rgw_name, this), false, true, false, m_cct);
  workers_data_lock = new Mutex(rgw_unique_lock_name(string("RGWBgtManager::workers_data_lock")+rgw_name, this), false, true, false, m_cct);
  lock = new Mutex(rgw_unique_lock_name(string("RGWBgtManager::lock")+rgw_name,this),false,true,false,m_cct);

  m_manager_inst_name = RGW_BGT_MANAGER_INST ;//+ std::string("_") + m_cct->_conf->name.to_str();
  m_merger_inst_name = RGW_BGT_MANAGER_MERGER_PREFIX + m_cct->_conf->name.to_str();

  m_manager_inst_obj = new RGWInstanceObj(_cct->_conf->rgw_primary_pool,m_manager_inst_name ,m_store, m_cct);
  if (NULL == m_manager_inst_obj)
  {
    ldout(m_cct , 5) << __func__ << " create RGWInstanceObj fail: " << m_cct->_conf->rgw_primary_pool << "/" <<  m_manager_inst_name << dendl;
    return -1;
  }
  ret = m_manager_inst_obj->init();
  if (ret < 0)
  {
    ldout(m_cct , 5) << __func__ << " RGWInstanceObj init fail: " << m_cct->_conf->rgw_primary_pool << "/" << m_manager_inst_name << dendl;
    delete m_manager_inst_obj;
    m_manager_inst_obj = NULL;
    return ret;
  }


  m_merger_inst_obj = new RGWInstanceObj(_cct->_conf->rgw_primary_pool,m_merger_inst_name ,m_store, m_cct);
  if (NULL == m_merger_inst_obj)
  {
    ldout(m_cct , 5) << __func__ << " create RGWInstanceObj fail: " << m_cct->_conf->rgw_primary_pool << "/" <<  m_merger_inst_name << dendl;
    return -1;
  }
  ret = m_merger_inst_obj->init();
  if (ret < 0)
  {
    ldout(m_cct , 5) << __func__ << " RGWInstanceObj init fail: " << m_cct->_conf->rgw_primary_pool << "/" << m_merger_inst_name << dendl;
    delete m_merger_inst_obj;
    m_merger_inst_obj = NULL;
    return ret;
  }
 

  //   
  //ldout(m_cct , 5) << "get scheduler and merger instance object" << dendl;
  get_scheduler_merger_info( );

  pre_check_worker_time = ceph_clock_now(0).sec();
  pre_reload_scheduler_info_time = ceph_clock_now(0).sec();
  pre_snap_v_time = ceph_clock_now(0).sec();
  //start check thread
  start( );
  return 0;
}


int RGWBgtManager::reload_scheduler( ) {

  int ret = 0;
  std::set<string> out_keys;
  ret = m_manager_inst_obj->m_io_ctx.omap_get_keys(m_manager_inst_name,"",LONG_MAX, &out_keys);
  ldout(m_cct , 5) << out_keys.size() << dendl;  

  std::set<std::string>::iterator it ;
  for(it = out_keys.begin() ; it != out_keys.end(); it++) {

    std::string key = *it;
    ldout(m_cct , 5) << *it << dendl;
    std::string scheduler_prefix = RGW_BGT_SCHEDULER_INST_PREFIX ;
    size_t pos = key.find(scheduler_prefix);
    if( pos != string::npos ) {
   
      ldout(m_cct , 5) << "check scheduler  :  " << *it << dendl;
      std::string  scheduler_name = key + std::string("_") + m_cct->_conf->name.to_str();
      std::map< std::string , RGWBgtScheduler*>::iterator s_it  = m_schedulers.find(scheduler_name);
      if( s_it == m_schedulers.end() )  {
         //rebuild a new scheduler
        ldout(m_cct , 5) << "rebuild a new scheduler  "<<*it << dendl;
        std :: set < std :: string > keys;
        std :: map < std :: string , bufferlist > vals;
        keys.insert(*it);
         
        ret = m_manager_inst_obj->m_io_ctx.omap_get_vals_by_keys(m_manager_inst_name, keys, &vals);
        if( 0 == ret ) { 
            
          ldout(m_cct , 5) << "vals size " << vals.size() << " key " << *it << dendl;
          bufferlist::iterator iter = vals[*it].begin();
          RGWSchedulerInfo info;
          ::decode(info,iter);
          ldout(m_cct , 5) << " value is "<< info.cold_pool << dendl;
            
          schedulers_data_lock->Lock();
          std :: map < std::string, RGWBgtScheduler*> :: iterator s_it = m_schedulers.find(key);
          if( s_it == m_schedulers.end() ) {
            RGWBgtScheduler* scheduler = new RGWBgtScheduler(m_store,m_cct,info.hot_pool, info.cold_pool);
            if(scheduler != NULL) {
               ret = scheduler->init( );
               m_schedulers.insert(std::pair<std::string, RGWBgtScheduler*>( scheduler->m_name, scheduler));
            }
          }
          schedulers_data_lock->Unlock( );
        }
      } 
      else {
        ldout(m_cct , 5) << (*it) << " is existed in scheduler_map ,refresh it "<< dendl;
        std :: set < std :: string > keys;
        std :: map < std :: string , bufferlist > vals;
        keys.insert(*it);

        ret = m_manager_inst_obj->m_io_ctx.omap_get_vals_by_keys(m_manager_inst_name, keys, &vals);
        if( 0 == ret ) {
          ldout(m_cct , 5) << "vals size " << vals.size() << " key " << *it << dendl;
          bufferlist::iterator iter = vals[*it].begin();
          RGWSchedulerInfo info;
          ::decode(info,iter);
          ldout(m_cct , 5) << " value is "<< info.cold_pool << dendl;
          RGWBgtScheduler* scheduler = s_it->second;
          //refresh it
          scheduler->cold_pool = info.cold_pool; 

          
          //schedulers_data_lock->Lock();
          //std :: map < std::string, RGWBgtScheduler*> :: iterator s_it = m_schedulers.find(key);
          //if( s_it == m_schedulers.end() ) {
          //  RGWBgtScheduler* scheduler = new RGWBgtScheduler(m_store,m_cct,info.hot_pool, info.cold_pool);
          //  if(scheduler != NULL) {
          //    ret = scheduler->init( );
          //    m_schedulers.insert(std::pair<std::string, RGWBgtScheduler*>( scheduler->m_name, scheduler));
          // }
          // }
          //schedulers_data_lock->Unlock( );

        }
                 

      }
    }
    else {
       
    }
  }
 
  return ret;
}

int RGWBgtManager::reload_workers( ) {

  int ret = 0;
  std::set<string> out_keys;
  out_keys.clear();
  ret = m_merger_inst_obj->m_io_ctx.omap_get_keys(m_merger_inst_name,"",LONG_MAX, &out_keys);
  std::set<std::string>::iterator it ;

  for(it = out_keys.begin() ; it != out_keys.end(); it++) {

    std::string key = *it;
    ldout(m_cct , 5) << *it << dendl;
    //size_t pos = key.find(RGW_BGT_SCHEDULER_INST_PREFIX);
    std::string scheduler_prefix = RGW_BGT_WORKER_INST_PREFIX ;
    size_t pos = key.find(scheduler_prefix);

    if( pos != string::npos ) {
      ldout(m_cct , 5) << "start worker" << key << dendl;
      workers_data_lock->Lock();
      std::map< std::string , RGWBgtWorker*>::iterator r_it = m_workers.find(key);
      if( r_it == m_workers.end() ) {
         
         RGWBgtWorker* worker = new RGWBgtWorker(m_store,m_cct,key);
         if(worker != NULL) {
           worker->init( );
           m_workers.insert(std::pair< std::string , RGWBgtWorker* >( worker->m_name, worker));
         }
      }
      workers_data_lock->Unlock( );
    }
  }//for
  b_reload_workers = true;
  return ret;
}

int RGWBgtManager::get_scheduler_info(std::string& key , RGWSchedulerInfo& info) {


  int r;
  std :: set < std :: string >keys;
  std :: map < std :: string,bufferlist > vals;
  keys.insert(key);
  r = m_manager_inst_obj->m_io_ctx.omap_get_vals_by_keys(m_manager_inst_name, keys, &vals);
  if (0 == r)
  {
    if (0 == vals[key].length())
    {
      ldout(m_cct , 5) << "don't found matched key "<< key << dendl;
      //batch_task_info.stage = RGW_BGT_BATCH_TASK_FINISH;
      //ldout(m_cct, 10) << "task " << task_name << " end "  << dendl;
    }
    else
    {
      bufferlist::iterator iter = vals[key].begin();
      ::decode(info, iter);
    }
  }
  else
  {
    ldout(m_cct , 0) << " load scheduler info failed : " << cpp_strerror(r) << dendl;
  }
  return r;
}


int RGWBgtManager::get_scheduler_merger_info( )
{
  int ret = 0;
 
  ret = reload_scheduler();

  if( ret != 0) {
    ldout(m_cct , 5) << "failed to reload scheduler" << dendl;
  }

  ret = reload_workers();

  if(ret != 0) {
    ldout(m_cct , 5) << "failed to reload workers" << dendl;
  }
  return ret;
}


RGWBgtScheduler* RGWBgtManager::get_adapter_scheduler(std::string& hot_pool) {
  
  schedulers_data_lock->Lock();
  RGWBgtScheduler* scheduler = NULL;
 // std::string key = RGW_BGT_SCHEDULER_INST_PREFIX + hot_pool;
  std::string key = RGW_BGT_SCHEDULER_INST_PREFIX + hot_pool + string("_") + m_cct->_conf->name.to_str();
  
  ldout(m_cct , 20) << "scheduler_name "<< key << dendl;
  std::map<std::string, RGWBgtScheduler*>::iterator it = m_schedulers.find(key);
  if(it != m_schedulers.end()) { 
    scheduler =  it->second;
  } 
  schedulers_data_lock->Unlock();
  return scheduler;
}

RGWBgtScheduler* RGWBgtManager::get_adapter_scheduler_from_name(std::string& scheduler_name) {
  
  schedulers_data_lock->Lock();
  RGWBgtScheduler* scheduler = NULL;
  std::map<std::string, RGWBgtScheduler*>::iterator it = m_schedulers.find(scheduler_name);
  if(it != m_schedulers.end()) { 
    scheduler =  it->second;
  } 
  schedulers_data_lock->Unlock();
  return scheduler;
}



int RGWBgtManager::gen_scheduler_instance(std::string& hot_pool, std::string& cold_pool, std::string& name) {
  
  int ret = 0 ;
  
  ldout(m_cct , 5) << "gen_scheduler_instance" << dendl;
  //judge scheduler'exist
  RGWBgtScheduler* scheduler = get_adapter_scheduler(hot_pool);
  if( scheduler != NULL ) {
    //restore scheduler info,check threads relaod scheduler_info
    ldout(m_cct , 5) << "scheduler instance " << hot_pool << " exist, refresh it" << dendl;
    std::string key_name;
    key_name = RGW_BGT_SCHEDULER_INST_PREFIX + hot_pool;
    bufferlist bl;
    std :: map < std :: string, bufferlist > values;
    RGWSchedulerInfo info;
    info.hot_pool = hot_pool;
    info.cold_pool = cold_pool;  
    info.name = name;
    ::encode(info,bl);
    values[key_name] = bl;
    ret = m_manager_inst_obj->m_io_ctx.omap_set(m_manager_inst_name , values ); 
    return ret;
  }

  //new scheduler instance
  RGWBgtScheduler* pScheduler = new RGWBgtScheduler(m_store, m_cct, hot_pool, cold_pool);
  if(pScheduler) {

  } 
  else {
    return -1;
  }

  std::string key_name;
  key_name = RGW_BGT_SCHEDULER_INST_PREFIX + hot_pool;

  //add it into omap
  bufferlist bl;
  std :: map < std :: string, bufferlist > values;
  RGWSchedulerInfo info;
  info.hot_pool = hot_pool;
  info.cold_pool = cold_pool;  
  info.name = name;
  ::encode(info,bl);
  values[key_name] = bl;
  ret = m_manager_inst_obj->m_io_ctx.omap_set(m_manager_inst_name , values ); 
  
  
  //add it into map
  schedulers_data_lock->Lock();
  ret = pScheduler->init( );
  if( ret < 0 ) {
    ldout(m_cct , 5) << "fail to init schedule" << pScheduler->m_name << dendl;
  }
  m_schedulers.insert(std::pair<std::string,RGWBgtScheduler*>(pScheduler->m_name,pScheduler));
  schedulers_data_lock->Unlock();
  return ret;
}
//
RGWBgtWorker*  RGWBgtManager::get_idle_merger_instance( ) {

 
  ldout(m_cct , 0) << "get idle merger instance from list" << dendl;
  RGWBgtWorker* worker = NULL;
  workers_data_lock->Lock();
  ldout(m_cct , 0) << "get_idle_merger_instance workers_data_lock.Lock" << dendl;
  ldout(m_cct , 0) << "get idle merger instance from list size = " << m_workers.size() << dendl;
  std::map<std::string, RGWBgtWorker*>::iterator it ;
  for(it = m_workers.begin(); it != m_workers.end(); it++ ) {
    RGWBgtWorker* tworker = it->second;
    ldout(m_cct , 0) << "get idle merger instance from list , worker_name  = " << tworker->m_name.c_str() << dendl;
    if( 1 == tworker->get_state() ) {
      worker = tworker;
      break;
    }
  }

 

  ldout(m_cct , 0) << "no found matched thread" << dendl;

  if( NULL == worker  && m_workers.size() <= MERGER_QUEUE_MEMBER_SIZE) {
    ldout(m_cct , 0) << "Generate a new merger instance" << dendl;
    RGWBgtWorker* tworker = gen_merger_instance( );
    tworker->init( );
    //worker->set_state(0);
    m_workers.insert(std::pair<std::string, RGWBgtWorker*>(tworker->m_name, tworker));
  }

  ldout(m_cct , 0) << "new merger instance" << dendl;
  if( NULL != worker ) {
    worker->set_state(0);
  }

  //ldout(m_cct , 0) << "return worker  : " << worker->m_name.c_str() << dendl;
  workers_data_lock->Unlock();
  ldout(m_cct , 0) << "get_idle_merger_instance workers_data_lock.Unlock" << dendl;
  return worker;
}  

RGWBgtWorker* RGWBgtManager::gen_merger_instance( ) {
  
  ldout(m_cct , 5) << "gen_merger_instance" << dendl;

  std::string  name = RGW_BGT_WORKER_INST_PREFIX + getUUID();
  RGWBgtWorker* worker = new RGWBgtWorker(m_store,m_cct, name);
  if(worker) {
    
  }
  else {
    return NULL;
  }
  
  //add it into omap
  bufferlist bl;
  std :: map < std :: string, bufferlist > values;
  ::encode(worker->m_name, bl);
  values[worker->m_name] = bl;
  m_merger_inst_obj->m_io_ctx.omap_set(m_merger_inst_name, values);
  //ldout(m_cct , 5) << "test log " << dendl;
  //m_workers.insert(std::pair<std::string, RGWBgtWorker*>(worker->m_name, worker));
  return worker;
}
    
 std::string RGWBgtManager::getUUID()
{
  struct uuid_d uuid;
  uuid.generate_random();
  char suuid[37];
  uuid.print(suuid);
  return std::string(suuid);
}


void RGWBgtManager::check_workers( ) {

  workers_data_lock->Lock();
  ldout(m_cct , 0) <<"check_workers workers_data_lock.Lock" << dendl;
  std::map< std::string , RGWBgtWorker*> :: iterator it ;
  ldout(m_cct , 0) << "start loop check workers , size = " << m_workers.size() << dendl;
  for(it = m_workers.begin() ; it != m_workers.end() ;  ) {
    ldout(m_cct , 0) << "check workers for loop" << dendl;
     RGWBgtWorker* worker = (*it).second;
     utime_t cur_time(ceph_clock_now(0));
     if(NULL != worker  && worker->get_state() == 1 &&  cur_time.sec() - worker->pre_idle_time.sec() >= WORKER_IDLE_TIMESPAN ) {

         ldout(m_cct , 0) << "find worker "<< worker->m_name << " , clear it" << dendl;
         worker->m_archive_task.stop();
         ldout(m_cct , 0) << "stop archive task" << dendl;
         worker->stop();
         ldout(m_cct , 0) << "stop worker" << dendl;
         worker->m_instobj->rm_ins_obj();
         ldout(m_cct , 0) << "rm_ins_obj " << dendl;
         worker->m_instobj->uninit();
         ldout(m_cct , 0) << "uninit " << dendl;
         std::set<std::string> keys;
         keys.insert(worker->m_name);
         
         m_merger_inst_obj->m_io_ctx.omap_rm_keys(m_merger_inst_name, keys);
         
         m_workers.erase(it++);
         delete worker;
     }
    else {
       it++;
    }
  }
  workers_data_lock->Unlock();  
  ldout(m_cct , 0) << "check_workers  workers_data_lock.Unlock" << dendl;
}

//snap scheduler v
void RGWBgtManager::snap_archive_v( ) {

  //uint64_t num = (uint64_t)archive_num.read();
  int size = archive_v_queue.size();
  if(size >= 11) {
     archive_v_queue.pop_front();
  }
  else {
    ldout(m_cct , 0) << " snap merger v "<< archive_num.read() << dendl;
    archive_v_queue.push_back( archive_num.read());
  }
  //
  std::list<uint64_t>::iterator it ;
  
  for(it = archive_v_queue.begin(); it != archive_v_queue.end(); it++ ) {
    ldout(m_cct , 5) << "merger v === " << (*it) << dendl;
  }
}

void* RGWBgtManager::entry() {

  while (!stopping)
  {
    ldout(m_cct , 5) << "RGWBgtManager::entry" << dendl;

    time_t cur_time = ceph_clock_now(0).sec();
    if(cur_time - pre_check_worker_time > WORKER_IDLE_TIMESPAN) { 
      ldout(m_cct , 5) << "start check workers" << dendl;
      check_workers();
      pre_check_worker_time = ceph_clock_now(0).sec();
    }

    cur_time = ceph_clock_now(0).sec();
    if(cur_time - pre_reload_scheduler_info_time > RELOAD_SCHEDULER_INFO_TIMESPAN ) {
      ldout(m_cct , 5) << "start reload schedulers" << dendl;
      reload_scheduler();
      pre_reload_scheduler_info_time = ceph_clock_now(0).sec();
    }

    cur_time = ceph_clock_now(0).sec();
    if(cur_time - pre_snap_v_time > SNAP_MERGER_V ) {
      snap_archive_v( );
      pre_snap_v_time = cur_time;
    }

    lock->Lock();
    cond.WaitInterval(NULL, *lock, utime_t(5, 0));
    lock->Unlock();
  }

  ldout(m_cct , 5) << "RGWBgtManager thread end" << dendl;

  return 0;
}

void RGWBgtManager::start()
{
  ldout(m_cct , 5) << "start check workers thread" << dendl;
  {
    Mutex::Locker l(*lock);
    stopping = false;
  }
  create();
}
void RGWBgtManager::stop()
{
  {
    Mutex::Locker l(*lock);
    stopping = true;
    cond.Signal();
  }

  join();
}




//==============================================================================
RGWManager::RGWManager() {

}


RGWManager::~RGWManager() {

}


int RGWManager::init(RGWRados* _store , CephContext* _cct) {

  int ret = 0;
  m_store = _store;
  m_cct = _cct;

  m_name = RGW_BGT_RGW_MANAGER_NAME ;

  m_instobj = new RGWInstanceObj(_cct->_conf->rgw_primary_pool,m_name ,m_store, m_cct);
  if (NULL == m_instobj)
  {
    ldout(m_cct, 0) << __func__ << " create RGWInstanceObj fail: " << m_cct->_conf->rgw_primary_pool << "/" <<  m_name << dendl;
    return -1;
  }
  ret = m_instobj->init();
  if (ret < 0)
  {
    ldout(m_cct, 0) << __func__ << " RGWInstanceObj init fail: " << m_cct->_conf->rgw_primary_pool << "/" << m_name << dendl;
    delete m_instobj;
    m_instobj = NULL;
    return ret;
  }
  //


  return 0;
}

std::string RGWManager::process_cmd( std::string& cmd, std::string& para1 , std::string& para2 ) {
  
   if(cmd == "scheduler create") {
      
      std::string hot_pool = para1;
      std::string cold_pool = para2;

   }


  return "";
}

bool RGWManager::notify_rgw( std::string& rgw_name, std::string& cmd ) {

  bool bret = false;
  ldout(m_cct , 5) << "notify rgw "<< rgw_name << " " << cmd << dendl;

  return bret;
}
