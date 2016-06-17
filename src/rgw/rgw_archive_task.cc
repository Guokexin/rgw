#include "rgw_archive_task.h"
#include "common/config.h"
#include "common/debug.h"
#include "common/dout.h"
#include "rgw_archive_op.h"
#include "rgw/rgw_rados.h"
#include <unistd.h>

#include <sstream>
#define dout_subsys ceph_subsys_rgw

void ArchiveTask::start(){
  // ===========test code==============
#if 0  
  int i = 0;
  for(i=0; i<1000; i++){
    obj_info* objinfo_00 = new obj_info();
    objinfo_00->obj_name = "abc.txt"; 
    objinfo_00->bucket_name = "first_bucket";
    archive_queue.push_back(objinfo_00);
  }
#endif  
  //====================================
  create();
}

void ArchiveTask::stop(){


    ldout(worker->m_cct , 5) << "ArchiveTask::stop 1" << dendl;
    sem_destroy(&sem_full);
    ldout(worker->m_cct , 5) << "ArchiveTask::stop 2" << dendl;
    task_archive_stop = true;
    ldout(worker->m_cct , 5) << "ArchiveTask::stop 3" << dendl;
    task_archive_lock.Lock();
    ldout(worker->m_cct , 5) << "ArchiveTask::stop 4" << dendl;
    task_type = 0;
    cond.Signal();
    ldout(worker->m_cct , 5) << "ArchiveTask::stop 5" << dendl;
    task_archive_lock.Unlock();
    ldout(worker->m_cct , 5) << "ArchiveTask::stop 6" << dendl;
    join();


 /*
    ldout(m_cct , 0) << "ArchiveTask Stop" << dendl;
    sem_destroy(&sem_full);
    task_archive_stop = true;
    join();
 */
}

void ArchiveTask::dispatch_task(int _type)
{
  ldout(this->worker->m_cct, 10) << " ===== Received a new task ===== " << dendl;
  task_archive_lock.Lock();
  task_type = _type;
  cond.Signal();
  task_archive_lock.Unlock();
}

void *ArchiveTask::task_thread_entry(){
  
  ldout(this->worker->m_cct, 0) << "======task thread start ============"<<dendl;
  

  while(!task_archive_stop)
  {
   
    ldout(worker->m_cct, 0) << " wait scheduler task "  <<dendl;
    task_archive_lock.Lock();
    cond.Wait(task_archive_lock);
    task_archive_lock.Unlock();

    if(task_type == 0) {
       break;
    }

    queue_size = worker->sfm_index.size();
    ldout(this->worker->m_cct, 0) << "=========start an task, total items:" << queue_size << dendl;
    if(queue_size == 0){
      ldout(cct, 0) << "recive a empty task, direct to return to worker" << dendl;
      worker->lock.Lock();
      //ldout(cct, 0) << "notify worker to work" << dendl;
      worker->cond.Signal();
      worker->lock.Unlock();
    }
    for( std::map <uint64_t, RGWSfmIndex>::iterator it = worker->sfm_index.begin(); it != worker->sfm_index.end(); it++)
    {
      ldout(worker->m_cct, 5) <<  "start process task " << dendl;
      sem_wait(&sem_full); 
      if( task_type == 1 ) {
        ldout(worker->m_cct,5) << "read task" << dendl;
        RGWReadArchiveOp *op = new RGWReadArchiveOp(this, it->first);
        if (NULL == op)
        {
          dout(0) << "allocate RGWReadArchiveOp instance failed." << dendl;
          //sem_post(&sem_full);
          list<bufferlist> lbl;
          handle_complete_task(lbl,0,it->first);  
          continue;
        }
        
        RGWSfmIndex *sfm_index = &it->second;
        sfm_index->result = true;
        int ret = op->pre_exec(store, sfm_index->bucket, sfm_index->bi_key);
        if(0 == ret) 
        {
          int nRet = op->execute();
          if( nRet != 0 ){
            ldout(worker->m_cct, 0) << "read task execute failed:" 
                                    << sfm_index->bucket << "/" 
                                    << sfm_index->oid << "/, "
                                    << sfm_index->bi_oid << "/"
                                    << sfm_index->bi_key << dendl;  
            sfm_index->result = false;
            //sem_post(&sem_full);
            list<bufferlist> lbl;
            handle_complete_task(lbl,0,it->first);
            delete op;
          }
        }
	      else
	      {
          ldout(worker->m_cct, 0) << "read task pre_exec failed:"  
                                  << sfm_index->bucket << "/" 
                                  << sfm_index->oid << "/, "
                                  << sfm_index->bi_oid << "/"
                                  << sfm_index->bi_key << dendl; 


          sfm_index->result = false;
	        //sem_post(&sem_full);
          list<bufferlist> lbl;
          handle_complete_task(lbl,0,it->first);
          delete op;
        }
      }
      else if( task_type == 2 ) 
      {
        if (!it->second.result)
        {
          //sem_post(&sem_full);
          list<bufferlist> lbl;
          handle_complete_task(lbl,0,it->first);  
          continue;          
        }
        
        dout(10) << "delete task, item=" << it->first << dendl;
        RGWDelArchiveOp *op = new RGWDelArchiveOp(this, it->first);
        if (NULL == op)
        {
          dout(0) << "allocate RGWReadArchiveOp instance failed." << dendl;
          it->second.result = false;
          //sem_post(&sem_full);
          list<bufferlist> lbl;
          handle_complete_task(lbl,0,it->first);  
          continue;
        }
        RGWSfmIndex *sfm_index = &it->second;
        list<bufferlist> lbl;
      
        int ret = op->pre_exec(store, sfm_index->bucket, sfm_index->bi_key);
        if(0 == ret)
        { 
          int nRet = op->execute(); 
          if( nRet != 0 )
          {
            ldout(worker->m_cct, 0) << "delete execute task failed:"
                                    << sfm_index->bucket << "/" 
                                    << sfm_index->oid << "/, "
                                    << sfm_index->bi_oid << "/"
                                    << sfm_index->bi_key << dendl; 


            sfm_index->result = false;
            //sem_post(&sem_full);
            list<bufferlist> lbl;
            handle_complete_task(lbl,0,it->first);
            delete op;
          }
        }  
        else 
        {
          ldout(worker->m_cct, 10) << "delete pre_exec task failed:"
                                   << sfm_index->bucket << "/" 
                                   << sfm_index->oid << "/, "
                                   << sfm_index->bi_oid << "/"
                                   << sfm_index->bi_key << dendl; 

          sfm_index->result = false;
          //sem_post(&sem_full);
          list<bufferlist> lbl;
          handle_complete_task(lbl,0,it->first);
          delete op;
        } 
      }
    } //for

    ldout(this->worker->m_cct, 0) << "=========task end========" << dendl;
  } //while

  ldout(this->worker->m_cct , 0) << "archive_task thread exit" << dendl;
  return 0;
}


void ArchiveTask::handle_complete_task(list<bufferlist>& lbl, uint64_t size, uint64_t item)
{
  task_archive_lock.Lock();
  int count = --queue_size;
  task_archive_lock.Unlock();
  ldout(this->worker->m_cct, 10) << "item=" << item << ", obj=" << worker->sfm_index[item].oid << ", leave count:"<< count << dendl;
  if(count == 0)
  {
    ldout(this->worker->m_cct, 0) << "task_type " << task_type <<" task_id = " << worker->task_id << dendl;
    if(task_type == 1){
      worker->sfm_index[item].lbl.splice(worker->sfm_index[item].lbl.end(), lbl);
    }


    sem_post(&sem_full);
    worker->lock.Lock();
    //ldout(cct, 0) << "notify worker to work" << dendl;
    worker->cond.Signal();
    worker->lock.Unlock();
    ldout(this->worker->m_cct, 0) << "Task end , Notify Schedule Role  " << dendl;
    
  }
  else
  {
    if( task_type == 1 ){
      worker->sfm_index[item].lbl.splice(worker->sfm_index[item].lbl.end(), lbl);
    }
    ldout(this->worker->m_cct, 10) << "release V signal and wait next op " << count <<dendl;
    sem_post(&sem_full);
  }
}
