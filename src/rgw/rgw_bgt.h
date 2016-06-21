// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGWBGT_H
#define CEPH_RGWBGT_H

#include <queue>
#include "include/rados/librados.hpp"
#if 0
#include "include/radosstriper/libradosstriper.hpp"
#endif
#include "include/Context.h"
#include "common/RefCountedObj.h"
#include "common/RWLock.h"
#include "rgw_common.h"
#include "cls/rgw/cls_rgw_types.h"
#include "cls/version/cls_version_types.h"
#include "cls/log/cls_log_types.h"
#include "cls/statelog/cls_statelog_types.h"
#include "rgw_log.h"
#include "rgw_metadata.h"
#include "rgw_rest_conn.h"
#include <boost/variant/variant.hpp>
#include "include/stringify.h"
#include "rgw_archive_task.h"

#define dout_subsys ceph_subsys_rgw

class RGWInstanceObj;
class RGWInstObjWatchCtx;
class RGWInstanceObjWatcher;
class RGWBgtScheduler;
class RGWBgtWorker;
class ArchiveTask;


#define RGW_BGT_MANAGER_INST "xsky.manager.archive.task.inst"
#define RGW_BGT_MANAGER_MERGER_PREFIX "xsky.manager.merger_" 
#define RGW_BGT_SCHEDULER_INST_PREFIX	"xsky.scheduler."
#define RGW_BGT_WORKER_INST_PREFIX "xsky.worker."
#define RGW_BGT_BATCH_TASK_META_KEY	"xsky.bgt_meta_batch_task"
#define RGW_BGT_TASK_META_KEY       "xsky.bgt_meta_task"
#define RGW_BGT_ACTIVE_CHANGE_LOG_KEY "xsky.bgt_active_change_log"
#define RGW_BGT_TASK_PREFIX "xsky.bgt_task_"
#define RGW_BGT_CHANGELOG_PREFIX "xsky.bgt_log_"
#define RGW_BGT_LOG_TRANS_META_KEY "xsky.bgt_meta_log_trans"
#define RGW_BGT_MERGEFILE_PREFIX "xsky.sfm_"
#define RGW_BGT_BATCH_INST_PREFIX "xsky.batch."
#define RGW_BGT_RGW_MANAGER_NAME  "xsky.bgt.rgw_manager"

#define RGW_ROLE_IO_PROCESS    0x1 
#define RGW_ROLE_BGT_SCHEDULER 0x2
#define RGW_ROLE_BGT_WORKER    0x4
#define RGW_ROLE_BGT_MERGER    0x8

#define RGW_INSTOBJ_NOTIFY_TIMEOUT 30000000
#define RGW_CHANGELOG_SHARDS_MAX 128
#define RGW_CHANGELOG_SHARDS_MOD 127


#define RGW_HOT_POOL   "ssd_pool"
#define RGW_COLD_POOL  "hdd_pool"


enum RGWInstObjWatchState {
	RGW_INSTOBJWATCH_STATE_UNREGISTERED,
	RGW_INSTOBJWATCH_STATE_REGISTERED,
	RGW_INSTOBJWATCH_STATE_ERROR
};

enum RGW_BGT_WORKER_STATE
{
	RGW_BGT_WORKER_INACTIVE					= 0,
	RGW_BGT_WORKER_ACTIVE						= 1	
};
std::ostream &operator<<(std::ostream &out, const RGW_BGT_WORKER_STATE &state);


enum RGWInstObjNotifyOp
{
	RGW_NOTIFY_OP_REGISTER							= 0,
	RGW_NOTIFY_OP_REGISTER_RSP					= 1,	
	RGW_NOTIFY_OP_ACTIVE_CHANGELOG 			= 2,
	RGW_NOTIFY_OP_ACTIVE_CHANGELOG_RSP 	= 3,
	RGW_NOTIFY_OP_DISPATCH_TASK					= 4,
	RGW_NOTIFY_OP_DISPATCH_TASK_RSP 		= 5,
	RGW_NOTIFY_OP_TASK_FINISH						= 6,
	RGW_NOTIFY_OP_TASK_FINISH_RSP				= 7
};
std::ostream &operator<<(std::ostream &out, const RGWInstObjNotifyOp &op);

std::string rgw_unique_lock_name(const std::string &name, void *address);

class RGWInstObjEncodePayloadVisitor : public boost::static_visitor<void> {
public:
  RGWInstObjEncodePayloadVisitor(bufferlist &bl) : m_bl(bl) {}

  template <typename Payload>
  inline void operator()(const Payload &payload) const {
    ::encode(static_cast<uint32_t>(Payload::NOTIFY_OP), m_bl);
    payload.encode(m_bl);
  }

private:
  bufferlist &m_bl;
};

class RGWInstObjDecodePayloadVisitor : public boost::static_visitor<void> {
public:
  RGWInstObjDecodePayloadVisitor(__u8 version, bufferlist::iterator &iter)
    : m_version(version), m_iter(iter) {}

  template <typename Payload>
  inline void operator()(Payload &payload) const {
    payload.decode(m_version, m_iter);
  }

private:
  __u8 m_version;
  bufferlist::iterator &m_iter;
};

class RGWInstObjDumpPayloadVisitor : public boost::static_visitor<void> {
public:
  RGWInstObjDumpPayloadVisitor(Formatter *formatter) : m_formatter(formatter) {}

  template <typename Payload>
  inline void operator()(const Payload &payload) const {
    RGWInstObjNotifyOp notify_op = Payload::NOTIFY_OP;
    m_formatter->dump_string("notify_op", stringify(notify_op));
    payload.dump(m_formatter);
  }

private:
  ceph::Formatter *m_formatter;
};

struct WorkerRegisterPayload
{
	static const RGWInstObjNotifyOp NOTIFY_OP = RGW_NOTIFY_OP_REGISTER;
	string worker_name;
	uint64_t task_id;
	//added by guokexin 20160322
  uint16_t role;
	//end

	WorkerRegisterPayload(){};
	//modified by guokexin 20160322
	//WorkerRegisterPayload(const string &name, uint64_t _task_id) : worker_name(name), task_id(_task_id){};
	WorkerRegisterPayload(const string &name, uint64_t _task_id,int _role) : worker_name(name), task_id(_task_id),role(_role){};
	
	void encode(bufferlist& bl) const
	{
		::encode(worker_name, bl);
		::encode(task_id, bl);
		::encode(role,bl);
	}
	void decode(__u8 version, bufferlist::iterator &iter)
	{
		::decode(worker_name, iter);
		::decode(task_id, iter);
		::decode(role, iter);
	}
	void dump(Formatter *f) const
	{
		f->dump_string("worker_name", worker_name);
		f->dump_unsigned("task_id", task_id);
		f->dump_unsigned("role", role);
	}
};

struct WorkerRegisterRspPayload
{
	static const RGWInstObjNotifyOp NOTIFY_OP = RGW_NOTIFY_OP_REGISTER_RSP;
	string active_change_log;


	WorkerRegisterRspPayload(){};
	WorkerRegisterRspPayload(const string &log_name) : active_change_log(log_name){};
	
	void encode(bufferlist& bl) const
	{
		::encode(active_change_log, bl);
	}
	void decode(__u8 version, bufferlist::iterator &iter)
	{
		::decode(active_change_log, iter);
	}
	void dump(Formatter *f) const
	{
		f->dump_string("active_change_log", active_change_log);
	}
};

struct ActiveChangeLogPayload
{
	static const RGWInstObjNotifyOp NOTIFY_OP = RGW_NOTIFY_OP_ACTIVE_CHANGELOG;
	string log_name;

	ActiveChangeLogPayload(){};
	ActiveChangeLogPayload(const string &name) : log_name(name){};
	
	void encode(bufferlist& bl) const
	{
		::encode(log_name, bl);
	}
	void decode(__u8 version, bufferlist::iterator &iter)
	{
		::decode(log_name, iter);
	}
	void dump(Formatter *f) const
	{
		f->dump_string("log_name", log_name);
	}	
};

struct ActiveChangeLogRspPayload
{
	static const RGWInstObjNotifyOp NOTIFY_OP = RGW_NOTIFY_OP_ACTIVE_CHANGELOG_RSP;
	string worker_name;

	ActiveChangeLogRspPayload(){};
	ActiveChangeLogRspPayload(const string &name) : worker_name(name){};
	
	void encode(bufferlist& bl) const
	{
		::encode(worker_name, bl);
	}
	void decode(__u8 version, bufferlist::iterator &iter)
	{
		::decode(worker_name, iter);
	}
	void dump(Formatter *f) const
	{
		f->dump_string("worker_name", worker_name);
	}	
};

struct DispatchTaskPayload
{
	static const RGWInstObjNotifyOp NOTIFY_OP = RGW_NOTIFY_OP_DISPATCH_TASK;
	uint64_t task_id;
	string log_name;
	uint32_t start_shard;
	uint64_t start;
	uint32_t end_shard;
	uint32_t count;	

	DispatchTaskPayload(){};
	DispatchTaskPayload(uint64_t _task_id, const string &_log_name, uint32_t _start_shard, uint64_t _start, uint32_t _end_shard, uint32_t _count) : 
		                        task_id(_task_id),
														log_name(_log_name),
														start_shard(_start_shard),
														start(_start),
														end_shard(_end_shard),
														count(_count)
{
};
	
	void encode(bufferlist& bl) const
	{
		::encode(task_id, bl);
		::encode(log_name, bl);
		::encode(start_shard, bl);
		::encode(start, bl);
		::encode(end_shard, bl);
		::encode(count, bl);
	}
	void decode(__u8 version, bufferlist::iterator &iter)
	{
		::decode(task_id, iter);
		::decode(log_name, iter);
		::decode(start_shard, iter);
		::decode(start, iter);
		::decode(end_shard, iter);
		::decode(count, iter);
	}
	void dump(Formatter *f) const
	{
		f->dump_unsigned("task_id", task_id);
		f->dump_string("log_name", log_name);
		f->dump_unsigned("start_shard", start_shard);
		f->dump_unsigned("start", start);
		f->dump_unsigned("end_shard", end_shard);
		f->dump_unsigned("count", count);
	}	
};

struct DispatchTaskRspPayload
{
	static const RGWInstObjNotifyOp NOTIFY_OP = RGW_NOTIFY_OP_DISPATCH_TASK_RSP;
	uint64_t task_id;
	string worker_name;

	DispatchTaskRspPayload(){};
	DispatchTaskRspPayload(uint64_t _task_id, const string &_worker_name) : 
		                            task_id(_task_id),
														    worker_name(_worker_name)
{
};
	
	void encode(bufferlist& bl) const
	{
		::encode(task_id, bl);
		::encode(worker_name, bl);
	}
	void decode(__u8 version, bufferlist::iterator &iter)
	{
		::decode(task_id, iter);
		::decode(worker_name, iter);
	}
	void dump(Formatter *f) const
	{
		f->dump_unsigned("task_id", task_id);
		f->dump_string("worker_name", worker_name);
	}	
};

struct TaskFinishedPayload
{
	static const RGWInstObjNotifyOp NOTIFY_OP = RGW_NOTIFY_OP_TASK_FINISH;
	uint64_t task_id;
	time_t finish_time;
  string worker_name;

	TaskFinishedPayload(){};
	TaskFinishedPayload(uint64_t _task_id, time_t _finish_time,string _work_name) : task_id(_task_id), finish_time(_finish_time),worker_name(_work_name){};
	
	void encode(bufferlist& bl) const
	{
		::encode(task_id, bl);
		::encode(finish_time, bl);
                ::encode(worker_name,bl);
	}
	void decode(__u8 version, bufferlist::iterator &iter)
	{
		::decode(task_id, iter);
		::decode(finish_time, iter);
                ::decode(worker_name,iter);
	}
	void dump(Formatter *f) const
	{
		f->dump_unsigned("task_id", task_id);
		f->dump_unsigned("finish_time", finish_time);
    f->dump_string("worker_name",worker_name);
	}		
};

struct TaskFinishedRspPayload
{
	static const RGWInstObjNotifyOp NOTIFY_OP = RGW_NOTIFY_OP_TASK_FINISH_RSP;
	uint64_t task_id;

	TaskFinishedRspPayload(){};
	TaskFinishedRspPayload(uint64_t _task_id) : task_id(_task_id){};
	
	void encode(bufferlist& bl) const
	{
		::encode(task_id, bl);
	}
	void decode(__u8 version, bufferlist::iterator &iter)
	{
		::decode(task_id, iter);
	}
	void dump(Formatter *f) const
	{
		f->dump_unsigned("task_id", task_id);
	}		
};

struct RGWInstObjUnknownPayload 
{
  static const RGWInstObjNotifyOp NOTIFY_OP = static_cast<RGWInstObjNotifyOp>(-1);

  void encode(bufferlist &bl) const
  {
  	assert(false);
  }
  void decode(__u8 version, bufferlist::iterator &iter)
  {
  }
  void dump(Formatter *f) const
  {
  }
};

typedef boost::variant<WorkerRegisterPayload,
                       ActiveChangeLogPayload,
                       DispatchTaskPayload,
                       TaskFinishedPayload,
                       RGWInstObjUnknownPayload> RGWInstObjPayload;


struct RGWInstObjNotifyMsg
{
	RGWInstObjPayload payload;

  RGWInstObjNotifyMsg() : payload(RGWInstObjUnknownPayload()) {}
  RGWInstObjNotifyMsg(const RGWInstObjPayload &payload_) : payload(payload_) {}	

  void encode(bufferlist& bl) const
  {
  	ENCODE_START(1, 1, bl);
		boost::apply_visitor(RGWInstObjEncodePayloadVisitor(bl), payload);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	DECODE_START(1, iter);

		uint32_t notify_op;
		::decode(notify_op, iter);

		switch (notify_op)
		{
			case RGW_NOTIFY_OP_REGISTER:
				payload = WorkerRegisterPayload();
				break;

			case RGW_NOTIFY_OP_ACTIVE_CHANGELOG:
				payload = ActiveChangeLogPayload();
				break;

			case RGW_NOTIFY_OP_DISPATCH_TASK:
				payload = DispatchTaskPayload();
				break;

			case RGW_NOTIFY_OP_TASK_FINISH:
                                /*guokexin...*/                     
				payload = TaskFinishedPayload();
				break;

			default:
				payload = RGWInstObjUnknownPayload();
				break;
		}

		apply_visitor(RGWInstObjDecodePayloadVisitor(struct_v, iter), payload);
		DECODE_FINISH(iter);
  }
	
  void dump(Formatter *f) const
  {
  	apply_visitor(RGWInstObjDumpPayloadVisitor(f), payload);
  }
};
WRITE_CLASS_ENCODER(RGWInstObjNotifyMsg);

std::ostream &operator<<(std::ostream &out, const RGWInstObjNotifyOp &op);


typedef boost::variant<WorkerRegisterRspPayload,
                       ActiveChangeLogRspPayload,
                       DispatchTaskRspPayload,
                       TaskFinishedRspPayload,
                       RGWInstObjUnknownPayload> RGWInstObjRspPayload;


struct RGWInstObjNotifyRspMsg
{
	int32_t result;
	RGWInstObjRspPayload payload;

  RGWInstObjNotifyRspMsg() : result(-1), payload(RGWInstObjUnknownPayload()) {}
  RGWInstObjNotifyRspMsg(const int32_t result_, const RGWInstObjRspPayload &payload_) : 
                                 result(result_), 
																 payload(payload_) {}	

  void encode(bufferlist& bl) const
  {
  	ENCODE_START(1, 1, bl);
		::encode(result, bl);
		boost::apply_visitor(RGWInstObjEncodePayloadVisitor(bl), payload);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	DECODE_START(1, iter);
		uint32_t notify_op;

		::decode(result, iter);
		::decode(notify_op, iter);

		switch (notify_op)
		{
			case RGW_NOTIFY_OP_REGISTER_RSP:
				payload = WorkerRegisterRspPayload();
				break;

			case RGW_NOTIFY_OP_ACTIVE_CHANGELOG_RSP:
				payload = ActiveChangeLogRspPayload();
				break;

			case RGW_NOTIFY_OP_DISPATCH_TASK_RSP:
				payload = DispatchTaskRspPayload();
				break;

			case RGW_NOTIFY_OP_TASK_FINISH_RSP:
				payload = TaskFinishedRspPayload();
				break;

			default:
				payload = RGWInstObjUnknownPayload();
				break;
		}
		
		apply_visitor(RGWInstObjDecodePayloadVisitor(struct_v, iter), payload);
		DECODE_FINISH(iter);
  }
	
  void dump(Formatter *f) const
  {
  	apply_visitor(RGWInstObjDumpPayloadVisitor(f), payload);
  }
};
WRITE_CLASS_ENCODER(RGWInstObjNotifyRspMsg);

struct RGWBgtTaskEntry
{
	uint32_t start_shard;
	uint32_t end_shard;
 	uint64_t start;
	uint32_t count;
	time_t dispatch_time;
	time_t finish_time;
	bool is_finished;

	RGWBgtTaskEntry(): start_shard(0), end_shard(0), start(0), count(0), 
		                       dispatch_time(utime_t()), finish_time(utime_t()), is_finished(false)
	{
	}
  void encode(bufferlist& bl) const
  {
  	ENCODE_START(1, 1, bl);
		::encode(start_shard, bl);
		::encode(end_shard, bl);
 		::encode(start, bl);
		::encode(count, bl);
		::encode(dispatch_time, bl);
		::encode(finish_time, bl);
		::encode(is_finished, bl);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	DECODE_START(1, iter);
		::decode(start_shard, iter);
		::decode(end_shard, iter);
 		::decode(start, iter);
		::decode(count, iter);
		::decode(dispatch_time, iter);
		::decode(finish_time, iter);
		::decode(is_finished, iter);
		DECODE_FINISH(iter);
  }
	
  void dump(Formatter *f) const
  {
  	f->dump_unsigned("start_shard", start_shard);
		f->dump_unsigned("end_shard", end_shard);
 		f->dump_unsigned("start", start);
		f->dump_unsigned("count", count);
		f->dump_unsigned("dispatch_time", dispatch_time);
		f->dump_unsigned("finish_time", finish_time);
		f->dump_bool("is_finished", is_finished);
	}
};
WRITE_CLASS_ENCODER(RGWBgtTaskEntry);

struct RGWBgtChangeLogInfo
{
	uint64_t order;
	uint64_t log_cnt;
	utime_t active_time;
	utime_t inactive_time;

	RGWBgtChangeLogInfo() : order(0), log_cnt(-1), active_time(utime_t()), inactive_time(utime_t())
	{
	}
	
  void encode(bufferlist& bl) const
  {
  	ENCODE_START(1, 1, bl);
		::encode(order, bl);
		::encode(log_cnt, bl);
		::encode(active_time, bl);
		::encode(inactive_time, bl);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	DECODE_START(1, iter);
		::decode(order, iter);
		::decode(log_cnt, iter);
		::decode(active_time, iter);
		::decode(inactive_time, iter);
		DECODE_FINISH(iter);
  }
	
  void dump(Formatter *f) const
  {
  	f->dump_unsigned("order", order);
		f->dump_unsigned("log_cnt", log_cnt);
		f->dump_unsigned("active_time", active_time);
		f->dump_unsigned("inactive_time", inactive_time);
	}
};
WRITE_CLASS_ENCODER(RGWBgtChangeLogInfo);

struct RGWBgtWorkerInfo
{
	RGW_BGT_WORKER_STATE state;
	uint16_t idle;	
	uint16_t role;

    
	RGWBgtWorkerInfo() : state(RGW_BGT_WORKER_INACTIVE),idle(0),role(0)
	{
	}
	
  void encode(bufferlist& bl) const
  {
  	uint8_t _state = state;
		uint8_t _role = role;
  	ENCODE_START(1, 1, bl);
		::encode(_state, bl);
		::encode(idle, bl);
		::encode(_role,bl);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	uint8_t _state;
		uint8_t _role;
  	DECODE_START(1, iter);
		::decode(_state, iter);
		::decode(idle, iter);
    ::decode(_role, iter);
		DECODE_FINISH(iter);
		state = static_cast<RGW_BGT_WORKER_STATE>(_state);
    role = static_cast<uint16_t>(_role);
  }
	
  void dump(Formatter *f) const
  {
  	f->dump_unsigned("state", state);
		f->dump_unsigned("idle", idle);
		f->dump_unsigned("role", role); 
	}
};
WRITE_CLASS_ENCODER(RGWBgtWorkerInfo);

enum RGWBgtTaskState
{
	RGW_BGT_TASK_WAIT_CREATE_SFMOBJ = 0,//wait to create big file obj
	RGW_BGT_TASK_WAIT_MERGE = 1, 				//wait to merge small file and write the big file
	RGW_BGT_TASK_WAIT_UPDATE_INDEX = 2, //wait to update meta of small file
	RGW_BGT_TASK_WAIT_DEL_DATA = 3,     //wait to delete data of small file  
	RGW_BGT_TASK_WAIT_REPORT_FINISH = 4,//wait to report to scheduler
	RGW_BGT_TASK_FINISH = 5							//task is finished
};
std::ostream &operator<<(std::ostream &out, const RGWBgtTaskState &state);

enum RGWBgtTaskSFMergeState
{
	RGW_BGT_TASK_SFM_START = 0,
	RGW_BGT_TASK_SFM_INDEX_CREATED = 1,
	RGW_BGT_TASK_SFM_MERGED = 2,
	RGW_BGT_TASK_SFM_DATA_FLUSHED = 3,
	RGW_BGT_TASK_SFM_INDEX_FLUSHED = 4
};
std::ostream &operator<<(std::ostream &out, const RGWBgtTaskSFMergeState &state);

struct RGWBgtTaskInfo
{
  RGWBgtTaskInfo():scheduler_name(""),hot_pool("") {
  }

	uint64_t task_id;
	string log_name;
	uint32_t start_shard;
	uint32_t end_shard;
	uint64_t start;
	uint32_t count;
	uint32_t sfm_obj_data_off;
	RGWBgtTaskState stage;
	string dst_pool;
	string dst_file;
  std::string scheduler_name;
  std::string hot_pool;

  void encode(bufferlist& bl) const
  {
  	uint8_t _stage = stage;
  	ENCODE_START(1, 1, bl);
		::encode(task_id, bl);
		::encode(log_name, bl);
		::encode(start_shard, bl);
		::encode(end_shard, bl);
		::encode(start, bl);
		::encode(count, bl);
		::encode(sfm_obj_data_off, bl);
		::encode(_stage, bl);
		::encode(dst_pool, bl);
		::encode(dst_file, bl);
    ::encode(scheduler_name,bl);
    ::encode(hot_pool,bl);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	uint8_t _stage;
  	DECODE_START(1, iter);
		::decode(task_id, iter);
		::decode(log_name, iter);
		::decode(start_shard, iter);
		::decode(end_shard, iter);
		::decode(start, iter);
		::decode(count, iter);
		::decode(sfm_obj_data_off, iter);
		::decode(_stage, iter);
		::decode(dst_pool, iter);
		::decode(dst_file, iter);
    ::decode(scheduler_name, iter);
    ::decode(hot_pool,iter);
		DECODE_FINISH(iter);
		stage = static_cast<RGWBgtTaskState>(_stage);
  }
	
  void dump(Formatter *f) const
  {
  	f->dump_unsigned("task_id", task_id);
		f->dump_string("log_name", log_name);
		f->dump_unsigned("start_shard", start_shard);
		f->dump_unsigned("end_shard", end_shard);
		f->dump_unsigned("start", start);
		f->dump_unsigned("count", count);
		f->dump_unsigned("sfm_obj_data_off", sfm_obj_data_off);
		f->dump_unsigned("stage", stage);
		f->dump_string("dst_pool", dst_pool);
		f->dump_string("dst_file", dst_file);
    f->dump_string("scheduler_name",scheduler_name);
    f->dump_string("hot_pool",hot_pool);
	}	
};
WRITE_CLASS_ENCODER(RGWBgtTaskInfo);

enum RGWBgtBatchTaskState
{
	RGW_BGT_BATCH_TASK_WAIT_CREATE = 0, 						//wait to create task
	RGW_BGT_BATCH_TASK_WAIT_DISPATCH = 1, 					//wait to dispatch task
	RGW_BGT_BATCH_TASK_WAIT_RM_LOG = 2,							//wait to clear log file
	RGW_BGT_BATCH_TASK_WAIT_RM_LOG_ENTRY = 3,				//wait to remove log entry in change logs
	RGW_BGT_BATCH_TASK_WAIT_CLEAR_TASK_ENTRY = 4,		//wait to clear task entry
	RGW_BGT_BATCH_TASK_FINISH = 5     				  		//batch task is finished
};
std::ostream &operator<<(std::ostream &out, const RGWBgtBatchTaskState &state);

enum RGWBgtLogTransState
{
	RGW_BGT_LOG_TRANS_START = 0,
	RGW_BGT_LOG_TRANS_WAIT_CREATE_LOG_OBJ = 1,
	RGW_BGT_LOG_TRANS_WAIT_ADD_TO_INACTIVE = 2,
	RGW_BGT_LOG_TRANS_WAIT_SET_ACTIVE = 3,
	RGW_BGT_LOG_TRANS_WAIT_NOTIFY_WORKER = 4,
	RGW_BGT_LOG_TRANS_FINISH = 5
};
std::ostream &operator<<(std::ostream &out, const RGWBgtLogTransState &state);

struct RGWBgtLogTransInfo
{
	RGWBgtLogTransState stage;
	string active_change_log;
	string pending_active_change_log;
	uint64_t order;
	utime_t active_time;

	RGWBgtLogTransInfo() : stage(RGW_BGT_LOG_TRANS_FINISH), 
		                             active_change_log(""),
		                             pending_active_change_log(""),
		                             order(0)
	{
	}
  void encode(bufferlist& bl) const
  {
  	uint8_t _stage = stage;
  	ENCODE_START(1, 1, bl);
		::encode(_stage, bl);
		::encode(active_change_log, bl);
		::encode(pending_active_change_log, bl);
		::encode(order, bl);
		::encode(active_time, bl);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	uint8_t _stage;
  	DECODE_START(1, iter);
		::decode(_stage, iter);
		::decode(active_change_log, iter);
		::decode(pending_active_change_log, iter);
		::decode(order, iter);
		::decode(active_time, iter);
		DECODE_FINISH(iter);
		stage = static_cast<RGWBgtLogTransState>(_stage);
  }
	
  void dump(Formatter *f) const
  {
		f->dump_unsigned("stage", stage);
		f->dump_string("active_change_log", active_change_log);
		f->dump_string("pending_active_change_log", pending_active_change_log);
		f->dump_unsigned("order", order);
		f->dump_unsigned("active_time", active_time);
	}		
};
WRITE_CLASS_ENCODER(RGWBgtLogTransInfo);

struct RGWChangeLogEntry
{
	string bucket;
	string bi_key;
	string bi_oid;
	uint64_t size;

	RGWChangeLogEntry() : bucket(""), bi_key(""), bi_oid(""), size(0)
  {
	}
  void encode(bufferlist& bl) const
  {
  	ENCODE_START(1, 1, bl);
		::encode(bucket, bl);
		::encode(bi_key, bl);
		::encode(bi_oid, bl);
		::encode(size, bl);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	DECODE_START(1, iter);
		::decode(bucket, iter);
		::decode(bi_key, iter);
		::decode(bi_oid, iter);
		::decode(size, iter);
		DECODE_FINISH(iter);
  }
	
  void dump(Formatter *f) const
  {
		f->dump_string("bucket", bucket);
		f->dump_string("bi_key", bi_key);
		f->dump_string("bi_oid", bi_oid);
		f->dump_unsigned("size", size);
	}	
};
WRITE_CLASS_ENCODER(RGWChangeLogEntry);

struct RGWChangeLogHdr
{
	uint32_t log_index_off;
	uint32_t log_index_cnt;

	RGWChangeLogHdr() : log_index_off(0), log_index_cnt(0)
	{
	}
};

struct RGWChangeLogIndex
{
	uint32_t off;
	uint32_t size;
	uint32_t obj_size;

	RGWChangeLogIndex() : off(0), size(0), obj_size(0)
	{
	}	
};

struct RGWSfmIndex
{
	librados::IoCtx *data_io_ctx;
	librados::IoCtx *index_io_ctx;
	string bucket;
	string oid;
	string bi_key;
	string bi_oid;
	uint64_t off;
	uint64_t size;
	list<bufferlist> lbl;
	bool result;

	RGWSfmIndex() : data_io_ctx(NULL),
									index_io_ctx(NULL),
									bucket(""),
									oid(""),
									bi_key(""),
									bi_oid(""),
									off(0),
									size(0),
									result(false)
	{
		lbl.clear();
	}
};

struct RGWSfmObjItemIndex
{
	uint32_t meta_off;
	uint32_t meta_size;
	uint32_t data_off;
	uint32_t data_size;

	RGWSfmObjItemIndex():meta_off(0), meta_size(0), data_off(0), data_size(0)
	{
	}
};

struct RGWSfmObjHeader
{
	uint32_t item_cnt;
	uint32_t data_off;
	RGWSfmObjItemIndex items[0];
};

struct RGWSfmObjItemMeta
{
	string bucket;
	string bi_key;
	string bi_oid;

	RGWSfmObjItemMeta() : bucket(""), bi_key(""), bi_oid("")
	{
	}
	
  void encode(bufferlist& bl) const
  {
  	ENCODE_START(1, 1, bl);
		::encode(bucket, bl);
		::encode(bi_key, bl);
		::encode(bi_oid, bl);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	DECODE_START(1, iter);
		::decode(bucket, iter);
		::decode(bi_key, iter);
		::decode(bi_oid, iter);
		DECODE_FINISH(iter);
  }
	
  void dump(Formatter *f) const
  {
		f->dump_string("bucket", bucket);
		f->dump_string("bi_key", bi_key);
		f->dump_string("bi_oid", bi_oid);
	}	
};
WRITE_CLASS_ENCODER(RGWSfmObjItemMeta);

struct RGWSfmIndexMeta
{
	uint32_t meta_off;
	uint32_t meta_size;
	uint32_t off;
	uint32_t size;
	bool     result;

	RGWSfmIndexMeta():meta_off(0), meta_size(0), off(0), size(0), result(false)
	{
	}
};

struct RGWSfmIndexHeader
{
	uint32_t item_cnt;
	RGWSfmIndexMeta items[0];
};

struct RGWBgtBatchTaskInfo
{
	string log_name;
	RGWBgtBatchTaskState stage;
	uint64_t task_cnt;
	uint64_t next_task;
	utime_t start_time;

  void encode(bufferlist& bl) const
  {
  	uint8_t _stage = stage;
  	ENCODE_START(1, 1, bl);
		::encode(log_name, bl);
		::encode(_stage, bl);
		::encode(task_cnt, bl);
		::encode(next_task, bl);
		::encode(start_time, bl);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	uint8_t _stage;
  	DECODE_START(1, iter);
		::decode(log_name, iter);
		::decode(_stage, iter);
		::decode(task_cnt, iter);
		::decode(next_task, iter);
		::decode(start_time, iter);
		DECODE_FINISH(iter);
		stage = static_cast<RGWBgtBatchTaskState>(_stage);
  }
	
  void dump(Formatter *f) const
  {
		f->dump_string("log_name", log_name);
		f->dump_unsigned("stage", stage);
		f->dump_unsigned("task_cnt", task_cnt);
		f->dump_unsigned("next_task", next_task);
		f->dump_unsigned("start_time", start_time);
	}		
};
WRITE_CLASS_ENCODER(RGWBgtBatchTaskInfo);

//added by guokexin 20160509 
struct RGWBatchInst {

	string log_name;
  string inst_name;
  void encode(bufferlist& bl) const
  {
  	ENCODE_START(1, 1, bl);
		::encode(log_name, bl);
    ::encode(inst_name, bl);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	DECODE_START(1, iter);
		::decode(log_name, iter);
    ::decode(inst_name, iter);
		DECODE_FINISH(iter);
  }
	
  void dump(Formatter *f) const
  {
		f->dump_string("log_name", log_name);
    f->dump_string("inst_name", inst_name);
	}		
};
WRITE_CLASS_ENCODER(RGWBatchInst);
//end added

class RGWInstObjWatchCtx : public librados::WatchCtx2 
{
public:

	RGWInstObjWatchCtx(RGWInstanceObjWatcher *parent, CephContext* cct) : 
		                        m_instobj_watcher(parent),
														m_cct(cct)	
														{}
  ~RGWInstObjWatchCtx(){}
	virtual void handle_notify(uint64_t notify_id,
																uint64_t handle,
														    uint64_t notifier_id,
																bufferlist& bl);
	virtual void handle_error(uint64_t handle, int err);
private:
	RGWInstanceObjWatcher *m_instobj_watcher;
	CephContext* m_cct;
};	

class RGWInstanceObj
{
	friend class RGWInstanceObjWatcher;
	friend class RGWInstObjWatchCtx;
	friend class RGWBgtWorker;
	friend class RGWBgtScheduler;
  friend class RGWBgtManager;
public:
  RGWInstanceObj(const std::string &pool_name, const std::string &instobj_name, RGWRados* store, CephContext* cct);
  ~RGWInstanceObj();	

	int init();
  int register_watch(uint64_t* handle, RGWInstObjWatchCtx* ctx);
  int unregister_watch(uint64_t handle);
  int uninit(); // Close our pool handle
  int rm_ins_obj( ) ; //delete ins obj
private:
  CephContext *m_cct;
	RGWRados *m_store;
	string m_pool;
	string m_name;
	librados::IoCtx m_io_ctx;
#if 0	
	libradosstriper::RadosStriper m_striper;
#endif
	RGWInstanceObjWatcher *m_instobj_watcher;	
};

struct C_RGWInstObjNotifyAck : public Context {
	RGWInstanceObjWatcher *inst_obj_watcher;
	uint64_t notify_id;
	uint64_t handle;
	bufferlist out;

	C_RGWInstObjNotifyAck(RGWInstanceObjWatcher *inst_obj_watcher, uint64_t notify_id, uint64_t handle);
	virtual void finish(int r);
};

struct C_RGWInstObjResponseMessage : public Context {
	C_RGWInstObjNotifyAck *notify_ack;

	C_RGWInstObjResponseMessage(C_RGWInstObjNotifyAck *notify_ack) : notify_ack(notify_ack) {
	}
	virtual void finish(int r);
};
#if 0
struct C_ProcessPayload : public Context {
	RGWInstanceObjWatcher *inst_obj_watcher;
	uint64_t notify_id;
	uint64_t handle;
	RGWInstObjPayload payload;

	C_ProcessPayload(RGWInstanceObjWatcher *inst_obj_watcher_, uint64_t notify_id_,
									      uint64_t handle_, const RGWInstObjPayload &payload_) : 
									      inst_obj_watcher(inst_obj_watcher_), 
												notify_id(notify_id_), 
												handle(handle_),
												payload(payload_) 
	{
	}

	virtual void finish(int r) override 
	{
		inst_obj_watcher->process_payload(notify_id, handle, payload, r);
	}
};
#endif

class RGWInstanceObjWatcher
{
	friend class RGWRados;
	friend class RGWInstObjWatchCtx;
	friend class C_RGWInstObjNotifyAck;

/* Begin added by hechuang */
class C_ReinitWatch : public Context {
  RGWInstanceObjWatcher *watcher;
  public:
	C_ReinitWatch(RGWInstanceObjWatcher *_watcher) : watcher(_watcher) {}
	void finish(int r) {
	  watcher->reinit();
	}
};	
/* End added */

	
public:
  RGWInstanceObjWatcher(RGWInstanceObj* instobj, int role, RGWRados* store, CephContext* cct);
  virtual ~RGWInstanceObjWatcher();

  /* Begin added by hechuang */
  void reinit();  
  /* End added */
  

  int register_watch();
  int unregister_watch();

	void handle_notify(uint64_t notify_id, uint64_t handle, bufferlist &bl); 
	void handle_error(uint64_t handle, int err);

	void process_payload(uint64_t notify_id, uint64_t handle, const RGWInstObjPayload &payload, int r);
	void acknowledge_notify(uint64_t notify_id, uint64_t handle, bufferlist &ack_bl);
	int get_notify_rsp(bufferlist& ack_bl, RGWInstObjNotifyRspMsg& notify_rsp);

	virtual int notify_register(uint64_t timeout_ms) {return 0;}
	virtual int notify_active_changelog(const string& worker_name, const string& log_name, uint64_t timeout_ms){return 0;}
	virtual int notify_dispatch_task(RGWBgtWorker* worker, RGWBgtBatchTaskInfo& batch_task_info, RGWBgtTaskEntry* _task_entry ){return 0;}
	virtual int notify_task_finished(uint64_t task_id, uint64_t timeout_ms){return 0;}

	virtual bool handle_payload(const WorkerRegisterPayload& payload,	C_RGWInstObjNotifyAck *ctx){return true;}
	virtual bool handle_payload(const ActiveChangeLogPayload& payload,	C_RGWInstObjNotifyAck *ctx){return true;}
	virtual bool handle_payload(const DispatchTaskPayload& payload,	C_RGWInstObjNotifyAck *ctx){return true;}
	virtual bool handle_payload(const TaskFinishedPayload& payload,	C_RGWInstObjNotifyAck *ctx){return true;}
	virtual bool handle_payload(const RGWInstObjUnknownPayload& payload,	C_RGWInstObjNotifyAck *ctx){return false;}

protected:
	CephContext* m_cct;
	RGWRados *m_store;
  RGWInstanceObj *m_instobj;
  RGWInstObjWatchCtx m_watch_ctx;
  uint64_t m_watch_handle;
	int m_watch_state;
	int m_state;
	int m_role;
};

struct RGWInstObjHandlePayloadVisitor : public boost::static_visitor<void> 
{
	RGWInstanceObjWatcher *inst_obj_watcher;
	uint64_t notify_id;
	uint64_t handle;

	RGWInstObjHandlePayloadVisitor(RGWInstanceObjWatcher *inst_obj_watcher_, uint64_t notify_id_, uint64_t handle_): 
		                        							inst_obj_watcher(inst_obj_watcher_), 
																					notify_id(notify_id_), 
																					handle(handle_)
	{
	}

	template <typename Payload>
	inline void operator()(const Payload &payload) const {
		C_RGWInstObjNotifyAck *ctx = new C_RGWInstObjNotifyAck(inst_obj_watcher, notify_id, handle);
		if (inst_obj_watcher->handle_payload(payload, ctx)) 
	  {
			ctx->complete(0);
		}
	}
};

class RGWBgtScheduler : public Thread
{

  friend class RGWBgtManager;
  friend class RGWBgtWorker;

public:
	RGWBgtScheduler(RGWRados* store, CephContext* cct, std::string& hot_pool, std::string& cold_pool );
	~RGWBgtScheduler();



	string unique_change_log_id();
	
  int init( );

	void load_change_logs();
	void load_active_change_log();
	void load_workers();
	void load_tasks();

	void update_active_change_log();
	int set_active_change_log(RGWBgtLogTransInfo& log_trans_info);
	int set_change_logs(RGWBgtLogTransInfo& log_trans_info);
	int set_log_trans_info(RGWBgtLogTransInfo& log_trans_info);
	int get_log_trans_info(RGWBgtLogTransInfo& log_trans_info);
	void check_log_trans();

  //added by guokexin 20160509
  int set_batch_inst(RGWBatchInst& inst);
  void load_batch_inst( );
  //add task_name by guokexin  20160509 
	int set_batch_task_info(RGWBgtBatchTaskInfo& batch_task_info , std::string& task_name);
	int get_batch_task_info(RGWBgtBatchTaskInfo& batch_task_info , std::string& task_name);
	void clear_task_entry(RGWBgtBatchTaskInfo& batch_task_info , std::string& task_name);
	void rm_log_entry(RGWBgtBatchTaskInfo& batch_task_info , std::string& task_name);
	void rm_change_log(RGWBgtBatchTaskInfo& batch_task_info , std::string & task_name);
	void dispatch_task(RGWBgtBatchTaskInfo& batch_task_info , std::string& task_name);

  //end added
  int notify_dispatch_task( RGWBgtWorker* worker, RGWBgtBatchTaskInfo& batch_task_info, RGWBgtTaskEntry* task_entry);
#if 0
	void mk_task_entry_from_buf(bufferlist& bl, uint64_t rs_cnt, 
                              uint32_t& next_start_shard, uint64_t& next_start, uint32_t& next_log_cnt,
                              uint32_t cur_shard, uint64_t& log_index, uint64_t& merge_size, 
                              uint64_t& task_id, bool is_last_step);
#endif
	void set_task_entries(RGWBgtBatchTaskInfo& batch_task_info,std::string& task_name);
	void mk_log_index_from_buf(bufferlist& log_buf_bl, 
				  													uint32_t& next_off, bufferlist& bl_log_index, 
																		uint32_t& log_index_cnt, int64_t& log_entry_size);
	int pre_process_change_log(string& log_name, uint32_t& log_cnt);
	int pre_process_change_log(string& log_name);
	void create_task(RGWBgtBatchTaskInfo& batch_task_info, std::string& task_name);
	int update_task_entry(uint64_t task_id, std::string& log_name);
	void check_batch_task();
  int write_change_log(RGWChangeLogEntry& log_entry);
	void dump_sf_remain(Formatter *f);
	void sf_force_merge(Formatter *f, string status);
	void sf_switch_batch_task(Formatter *f, string status);
	atomic_t force_merge_all;
	atomic_t batch_task_switch;



  int process_finished_task(RGWBgtWorker* worker,uint64_t task_id , std::string& log_name );
  //void set_name ( std::string& _name) {  m_name = _name;}
  //std::string get_name( ) { return m_name;  } 
   
	//Thread
	void *entry();

	void start();
	void stop();	



protected:
  
  RGWInstanceObj* m_instobj;
  RGWRados* m_store;
  CephContext* m_cct;
  bool stopping;
	bool wait_task_finish;
  Mutex lock;
  Cond cond;
	Mutex worker_data_lock;
	RWLock active_change_log_lock;
	atomic64_t max_log_id;
	string active_change_log;
  std::string hot_pool;
  std::string cold_pool;
	RWLock update_change_log_lock;
	atomic64_t change_log_num;
  std::string m_name;
  std::string scheduler_prefix;
  std::string public_active_change_log;
	std :: map < std :: string, RGWBgtChangeLogInfo> change_logs;
	std :: map < uint64_t, std :: string> order_change_logs;
  
	//std :: map < std :: string, RGWBgtWorkerInfo> workers;
	//std :: map < uint64_t, RGWBgtTaskEntry> batch_tasks;
  //processing batch task
  std :: map<std::string ,RGWBatchInst> m_processing_task_inst;  
  //
  std :: map<std::string, std::map<uint64_t, RGWBgtTaskEntry> > m_log_task_entry;
};

class RGWBgtWorker : public Thread
{
	friend class ArchiveTask;
	friend class RGWRados;
	friend class RGWArchiveOp;
  friend class RGWBgtManager;
  friend class RGWBgtScheduler;
public:
	RGWBgtWorker(RGWRados* store, CephContext* cct ,std::string& name);
	~RGWBgtWorker() 
	{
	}

	string unique_sfm_oid();
	int create_sfm_obj(RGWBgtTaskInfo& task_info);
	
	int set_task_info(RGWBgtTaskInfo& task_info);
	int get_task_info(RGWBgtTaskInfo& task_info);
	void report_task_finished(RGWBgtTaskInfo& task_info);
	int mk_sfm_index_from_change_log(bufferlist& bl, uint64_t rs_cnt, uint64_t& index_id);
	int create_sfm_index(RGWBgtTaskInfo& task_info);
	int mk_sfm_index_from_buf(RGWSfmIndexMeta* sfm_index_meta, bufferlist& bl, uint64_t rs_cnt, uint64_t& index_id);
	int load_sfm_index(RGWBgtTaskInfo & task_info);
	
	void sfm_flush_data(RGWBgtTaskInfo& task_info);
	void sfm_flush_index(RGWBgtTaskInfo& task_info);
	void set_sfm_result(RGWBgtTaskInfo& task_info);
	void process_merge_result(RGWBgtTaskInfo& task_info);
	void wait_merge(RGWBgtTaskInfo& task_info);
	void update_index(RGWBgtTaskInfo& task_info);
	void delete_data(RGWBgtTaskInfo& task_info);
	void load_task();
	void check_task();


  int notify_task_finished(std::string& name , uint64_t task_id, std::string& log_name); 
	//int write_change_log(RGWChangeLogEntry& log_entry);
  int init( );	
  int uninit( );
	//Thread
	void *entry();
	void start();
	void stop();
  //get worker state 1 idle 0 busy
  int get_state();
  void set_state(int _idle);
  //
  //
  int start_process_task(RGWBgtScheduler* scheduler, RGWBgtBatchTaskInfo& batch_task_info ,RGWBgtTaskEntry* _task_entry ); 
protected:
  RGWInstanceObj* m_instobj;
  RGWRados* m_store;
  CephContext* m_cct;
  bool stopping;
	bool is_ready_for_accept_task;
  std::string m_name;
  Mutex lock;
  Cond cond;	
	atomic64_t max_sfm_id;
	string active_change_log;
	librados::IoCtx *m_sfm_io_ctx;
#if 0	
	libradosstriper::RadosStriper *m_sfm_striper;
#endif
	std :: map < string, librados::IoCtx> data_io_ctx;
	std :: map < string, librados::IoCtx> index_io_ctx;

	RGWBgtTaskSFMergeState merge_stage;
	uint32_t sfm_obj_data_off;
	std :: map < uint64_t, RGWSfmIndex> sfm_index;
	bufferlist sfm_bl;
	ArchiveTask m_archive_task;
  int idle;
  utime_t pre_idle_time; // participate in mergering time
  //process match log file
  librados::IoCtx *m_log_io_ctx;
  //log pre hot_pool
  std::string hot_pool; 
 // RGWBgtScheduler* scheduler;
  Mutex lock1;
  Cond cond1;	
 
public:
  uint64_t  task_id;
public:
	int m_state;
  
};





struct RGWSchedulerInfo
{
	string hot_pool;
  string cold_pool;
  string name;

  void encode(bufferlist& bl) const
  {
  	ENCODE_START(1, 1, bl);
		::encode(hot_pool, bl);
		::encode(cold_pool, bl);
    ::encode(name,bl);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	DECODE_START(1, iter);
		::decode(hot_pool, iter);
		::decode(cold_pool, iter);
    ::decode(name,iter);
		DECODE_FINISH(iter);
  }
	
  void dump(Formatter *f) const
  {
		f->dump_string("hot_pool", hot_pool);
		f->dump_string("cold_pool", cold_pool);
    f->dump_string("name",name);
	}		
};
WRITE_CLASS_ENCODER(RGWSchedulerInfo);








class RGWBgtManager : public Thread {

  friend class RGWBgtWorker;
  friend class RGWBgtScheduler;
  private:
    RGWBgtManager( ):m_manager_inst_obj(NULL),m_merger_inst_obj(NULL),app_num(0),worker_num(0),m_store(NULL),m_cct(NULL),archive_num(0),stopping(true),b_reload_workers(false)
     { 
       
     };
 
    virtual ~RGWBgtManager() { 
      stopping = true;
      if(schedulers_data_lock != NULL) {
        delete schedulers_data_lock ;
        schedulers_data_lock = NULL;
      }
       
      if(workers_data_lock != NULL ) {
        delete workers_data_lock;
        workers_data_lock = NULL;
      }

      if(lock != NULL ) {
        delete lock;
        lock = NULL;
      }

    } ;
  private:
    static RGWBgtManager m_singleton;
  public:
    static RGWBgtManager* instance( );
    int init(RGWRados* _store, CephContext* _cct ); //init rgw_manager instance

  private:
    
    RGWInstanceObj* m_manager_inst_obj;
    RGWInstanceObj* m_merger_inst_obj;
    //key hot_pool, val : scheduler的实例
    std::map<std::string, RGWBgtScheduler*>  m_schedulers;
    //key merger_name, val: merger的实例
    std::map<std::string, RGWBgtWorker*> m_workers;
    //try to apply RGWBgtWorker instane, if beyond 3 times, gen a new merger instance
	  int app_num;
	  int worker_num;
    //
    RGWRados* m_store;
    CephContext* m_cct;
    std::string m_manager_inst_name;
    std::string m_merger_inst_name;
   
	  Mutex*  schedulers_data_lock;
    Mutex*  workers_data_lock;
    Mutex*  lock;
    Mutex*  merger_speed_lock;
    time_t  pre_check_worker_time;
    time_t  pre_reload_scheduler_info_time;
    atomic64_t  archive_num;
    std::list<uint64_t>  archive_v_queue;
    std::vector<uint64_t>  merger_v_ret;
    time_t pre_snap_v_time;
    
  //private:
  //  void gen_merger_name(std::string& name);
  public:
    //get a scheduler according to para1(hot_pool)
    //hot_pool : scheduler = 1:1  on the single process.
    RGWBgtScheduler* get_adapter_scheduler(std::string& hot_pool);
    RGWBgtScheduler* get_adapter_scheduler_from_name(std::string& scheduler_name);
    //product a scheduler instance, succ return 0 , fail return -1
    int gen_scheduler_instance(std::string& hot_pool, std::string& cold_pool, std::string& scheduler_name);
    //get a worker instance pointer according to para1 (merger_name)
    RGWBgtWorker* get_adapter_merger(std::string& merge_name);
    
    //get a idle worker instance pointer from m_workers
    //if exist idle merger, return it
    //else gen a instance ,return it
    RGWBgtWorker* get_idle_merger_instance( ) ;  
    //gen a merger instance and add it into m_workers,
    //succ to return it , fail return NULL
    RGWBgtWorker* gen_merger_instance( );
    //get scheduler merger info
    int get_scheduler_merger_info( );
    
    std::string getUUID( );
    
    int get_scheduler_info(std::string& key, RGWSchedulerInfo& info);
    int reload_scheduler( );
    int reload_workers( );
    void snap_archive_v( );
    void  gen_merger_speed(vector<uint64_t>& vec);
    //thread function implement
   public:
    void *entry();
    void start();
    void stop();
    void check_workers( );
    bool exist_idle_merge();
   private:
    bool stopping; 
    Cond cond;

  public:
    bool b_reload_workers;
};


//archive server struct
struct RGWArchiveServer
{
  std::string archive_name;
  std::string hot_pool;
  std::string cold_pool;
  
  void encode(bufferlist& bl) const
  {
  	ENCODE_START(1, 1, bl);
		::encode(archive_name, bl);
		::encode(hot_pool, bl);
		::encode(cold_pool, bl);
		ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& iter)
  {
  	DECODE_START(1, iter);
		::decode(archive_name, iter);
		::decode(hot_pool, iter);
		::decode(cold_pool, iter);
		DECODE_FINISH(iter);
  }
	
  void dump(Formatter *f) const
  {
		f->dump_string("archive_name", archive_name);
		f->dump_string("hot_pool", hot_pool);
		f->dump_string("cold_pool", cold_pool);
	}		
};
WRITE_CLASS_ENCODER(RGWArchiveServer);



/*****************************************************
 *manager all RadosGW and ArchiveServer
 ****************************************************/
class RGWManager {

  public:
    RGWManager();
    ~RGWManager();

  public:
    //init rgw_manager ,create inst_obj
    //append RadosGW name to inst_obj
    int init(RGWRados* _store , CephContext* _cct );

  private:
    RGWRados* m_store;
    CephContext* m_cct;
    std::string m_name;

    RGWInstanceObj* m_instobj;

    std::string process_cmd(std::string& cmd , std::string& para1, std::string& para2 ) ;
    bool notify_rgw(std::string&  cmd , std::string&  para1);
 
};
#endif
  
