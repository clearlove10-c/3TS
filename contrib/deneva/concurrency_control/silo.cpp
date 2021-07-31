/* Tencent is pleased to support the open source community by making 3TS available.
 *
 * Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved. The below software
 * in this distribution may have been modified by THL A29 Limited ("Tencent Modifications"). All
 * Tencent Modifications are Copyright (C) THL A29 Limited.
 *
 * Author: zhanhaozhao@ruc.edu.cn
 *
 */

#include "txn.h"
#include "row.h"
#include "silo.h"
#include "row_silo.h"

#if CC_ALG == SILO

RC
Silo::validate_silo(TxnManager * txn)
{
    RC rc = RCOK;
    // lock write tuples in the primary key order.
    uint64_t wr_cnt = txn->write_cnt;
    // write_set = (int *) mem_allocator.alloc(sizeof(int) * wr_cnt);
    int cur_wr_idx = 0;
    int read_set[txn->row_cnt - txn->write_cnt];
    int cur_rd_idx = 0;
    for (uint64_t rid = 0; rid < txn->row_cnt; rid ++) {
        if (txn->accesses[rid]->type == WR)
            write_set[cur_wr_idx ++] = rid;
        else 
            read_set[cur_rd_idx ++] = rid;
    }

    // bubble sort the write_set, in primary key order 
    if (wr_cnt > 1)
    {
        for (uint64_t i = wr_cnt - 1; i >= 1; i--) {
            for (uint64_t j = 0; j < i; j++) {
                if (txn->accesses[ write_set[j] ]->orig_row->get_primary_key() > 
                    txn->accesses[ write_set[j + 1] ]->orig_row->get_primary_key())
                {
                    int tmp = write_set[j];
                    write_set[j] = write_set[j+1];
                    write_set[j+1] = tmp;
                }
            }
        }
    }

    num_locks = 0;
    ts_t max_tid = 0;
    bool done = false;
    if (_pre_abort) {
        for (uint64_t i = 0; i < wr_cnt; i++) {
            row_t * row = txn->accesses[ write_set[i] ]->orig_row;
            if (row->manager->get_tid() != txn->accesses[write_set[i]]->tid) {
                rc = Abort;
                return rc;
            }    
        }    
        for (uint64_t i = 0; i < txn->row_cnt - wr_cnt; i ++) {
            Access * access = txn->accesses[ read_set[i] ];
            if (access->orig_row->manager->get_tid() != txn->accesses[read_set[i]]->tid) {
                rc = Abort;
                return rc;
            }
        }
    }

    // lock all rows in the write set.
    if (_validation_no_wait) {
        while (!done) {
            num_locks = 0;
            for (uint64_t i = 0; i < wr_cnt; i++) {
                row_t * row = txn->accesses[ write_set[i] ]->orig_row;
                if (!row->manager->try_lock())
                {
                    break;
                }
                DEBUG("silo %ld write lock row %ld \n", this->get_txn_id(), row->get_primary_key());
                row->manager->assert_lock();
                num_locks ++;
                if (row->manager->get_tid() != txn->accesses[write_set[i]]->tid)
                {
                    rc = Abort;
                    return rc;
                }
            }
            if (num_locks == wr_cnt) {
                DEBUG("TRY LOCK true %ld\n", get_txn_id());
                done = true;
            } else {
                rc = Abort;
                return rc;
            }
        }
    } else {
        for (uint64_t i = 0; i < wr_cnt; i++) {
            row_t * row = txn->accesses[ write_set[i] ]->orig_row;
            row->manager->lock();
            DEBUG("silo %ld write lock row %ld \n", txn->get_txn_id(), row->get_primary_key());
            num_locks++;
            if (row->manager->get_tid() != txn->accesses[write_set[i]]->tid) {
                rc = Abort;
                return rc;
            }
        }
    }

    uint64_t lower = time_table.get_lower(txn->get_thd_id(),txn->get_txn_id());
    uint64_t upper = time_table.get_upper(txn->get_thd_id(),txn->get_txn_id());
    DEBUG("MAAT Validate Start %ld: [%lu,%lu]\n",txn->get_txn_id(),lower,upper);
    std::set<uint64_t> after;
    std::set<uint64_t> before;

    // lower bound of txn greater than write timestamp
    if(lower <= txn->greatest_write_timestamp) {
        lower = txn->greatest_write_timestamp + 1;
        INC_STATS(txn->get_thd_id(),maat_case1_cnt,1);
    }
    // lower bound of txn greater than read timestamp
    if(lower <= txn->greatest_read_timestamp) {
        lower = txn->greatest_read_timestamp + 1;
        INC_STATS(txn->get_thd_id(),maat_case3_cnt,1);
    }

    COMPILER_BARRIER

    //RW
    // lower bound of uncommitted writes greater than upper bound of txn
    for(auto it = txn->uncommitted_writes->begin(); it != txn->uncommitted_writes->end();it++) {
        uint64_t it_lower = time_table.get_lower(txn->get_thd_id(),*it);
        if(upper >= it_lower) {
            SILOState state = time_table.get_state(txn->get_thd_id(),*it); //TODO
            if(state == SILO_VALIDATED || state == SILO_COMMITTED) {
                INC_STATS(txn->get_thd_id(),maat_case2_cnt,1);
                if(it_lower > 0) {
                upper = it_lower - 1;
                } else {
                upper = it_lower;
                }
            }
            if(state == SILO_RUNNING) {
                after.insert(*it);
            }
        }
    }

    // // validate rows in the read set
    // // for repeatable_read, no need to validate the read set.
    // for (uint64_t i = 0; i < txn->row_cnt - wr_cnt; i ++) {
    //     Access * access = txn->accesses[ read_set[i] ];
    //     //bool success = access->orig_row->manager->validate(access->tid, false);
    //     if (!success) {
    //         rc = Abort;
    //         return rc;
    //     }
    //     if (access->tid > max_tid)
    //         max_tid = access->tid;
    // }
    // validate rows in the write set


    //WW
    // upper bound of uncommitted write writes less than lower bound of txn
    for(auto it = txn->uncommitted_writes_y->begin(); it != txn->uncommitted_writes_y->end();it++) {
        SILOState state = time_table.get_state(txn->get_thd_id(),*it);
        uint64_t it_upper = time_table.get_upper(txn->get_thd_id(),*it);
        if(state == SILO_ABORTED) {
            continue;
        }
        if(state == SILO_VALIDATED || state == SILO_COMMITTED) {
            if(lower <= it_upper) {
            INC_STATS(txn->get_thd_id(),maat_case5_cnt,1);
            if(it_upper < UINT64_MAX) {
                lower = it_upper + 1;
            } else {
                lower = it_upper;
            }
            }
        }
        if(state == SILO_RUNNING) {
            after.insert(*it);
        }
    }

    //TODO: need to consider whether need WR check or not

    // for (uint64_t i = 0; i < wr_cnt; i++) {
    //     Access * access = txn->accesses[ write_set[i] ];
    //     bool success = access->orig_row->manager->validate(access->tid, true);
    //     if (!success) {
    //         rc = Abort;
    //         return rc;
    //     }
    //     if (access->tid > max_tid)
    //         max_tid = access->tid;
    // }

    // this->max_tid = max_tid;

    if(lower >= upper) {
        // Abort
        time_table.set_state(txn->get_thd_id(),txn->get_txn_id(),SILO_ABORTED);
        rc = Abort;
    } else {
        // Validated
        time_table.set_state(txn->get_thd_id(),txn->get_txn_id(),SILO_VALIDATED);
        rc = RCOK;

        for(auto it = before.begin(); it != before.end();it++) {
            uint64_t it_upper = time_table.get_upper(txn->get_thd_id(),*it);
            if(it_upper > lower && it_upper < upper-1) {
                lower = it_upper + 1;
            }
        }
        for(auto it = before.begin(); it != before.end();it++) {
            uint64_t it_upper = time_table.get_upper(txn->get_thd_id(),*it);
            if(it_upper >= lower) {
                if(lower > 0) {
                time_table.set_upper(txn->get_thd_id(),*it,lower-1);
                } else {
                time_table.set_upper(txn->get_thd_id(),*it,lower);
                }
            }
        }
        for(auto it = after.begin(); it != after.end();it++) {
            uint64_t it_lower = time_table.get_lower(txn->get_thd_id(),*it);
            uint64_t it_upper = time_table.get_upper(txn->get_thd_id(),*it);
            if(it_upper != UINT64_MAX && it_upper > lower + 2 && it_upper < upper ) {
                upper = it_upper - 2;
            }
            if((it_lower < upper && it_lower > lower+1)) {
                upper = it_lower - 1;
            }
        }
        // set all upper and lower bounds to meet inequality
        for(auto it = after.begin(); it != after.end();it++) {
            uint64_t it_lower = time_table.get_lower(txn->get_thd_id(),*it);
            if(it_lower <= upper) {
                if(upper < UINT64_MAX) {
                time_table.set_lower(txn->get_thd_id(),*it,upper+1);
                } else {
                time_table.set_lower(txn->get_thd_id(),*it,upper);
                }
            }
        }

        assert(lower < upper);
        INC_STATS(txn->get_thd_id(),maat_range,upper-lower);
        INC_STATS(txn->get_thd_id(),maat_commit_cnt,1);
    }
    time_table.set_lower(txn->get_thd_id(),txn->get_txn_id(),lower);
    time_table.set_upper(txn->get_thd_id(),txn->get_txn_id(),upper);
    DEBUG("MAAT Validate End %ld: %d [%lu,%lu]\n",txn->get_txn_id(),rc==RCOK,lower,upper);

    return rc;
}

RC
TxnManager::finish(RC rc)
{
    if (rc == Abort) {
        if (this->num_locks > get_access_cnt()) 
            return rc;
        for (uint64_t i = 0; i < this->num_locks; i++) {
            txn->accesses[ write_set[i] ]->orig_row->manager->release();
            DEBUG("silo %ld abort release row %ld \n", this->get_txn_id(), txn->accesses[ write_set[i] ]->orig_row->get_primary_key());
        }
    } else {
        
        for (uint64_t i = 0; i < txn->write_cnt; i++) {
            Access * access = txn->accesses[ write_set[i] ];
            access->orig_row->manager->write( 
                access->data, this->commit_timestamp );
            txn->accesses[ write_set[i] ]->orig_row->manager->release();
            DEBUG("silo %ld commit release row %ld \n", this->get_txn_id(), txn->accesses[ write_set[i] ]->orig_row->get_primary_key());
        }
    }
    num_locks = 0;
    memset(write_set, 0, 100);

    return rc;
}

RC
TxnManager::find_tid_silo(ts_t max_tid)
{
    if (max_tid > _cur_tid)
        _cur_tid = max_tid;
    return RCOK;
}

#endif

RC Silo::find_bound(TxnManager * txn) {
    RC rc = RCOK;
    uint64_t lower = time_table.get_lower(txn->get_thd_id(),txn->get_txn_id());
    uint64_t upper = time_table.get_upper(txn->get_thd_id(),txn->get_txn_id());
    if(lower >= upper) {
        time_table.set_state(txn->get_thd_id(),txn->get_txn_id(),SILO_VALIDATED);
        rc = Abort;
    } else {
        time_table.set_state(txn->get_thd_id(),txn->get_txn_id(),SILO_COMMITTED);
        // TODO: can commit_time be selected in a smarter way?
        txn->commit_timestamp = lower;
    }
    DEBUG("MAAT Bound %ld: %d [%lu,%lu] %lu\n", txn->get_txn_id(), rc, lower, upper,
            txn->commit_timestamp);
    return rc;
}

void TimeTable::init() {
    //table_size = g_inflight_max * g_node_cnt * 2 + 1;
    table_size = g_inflight_max + 1;
    DEBUG_M("TimeTable::init table alloc\n");
    table = (TimeTableNode*) mem_allocator.alloc(sizeof(TimeTableNode) * table_size);
    for(uint64_t i = 0; i < table_size;i++) {
        table[i].init();
    }
}

uint64_t TimeTable::hash(uint64_t key) { return key % table_size; }

TimeTableEntry* TimeTable::find(uint64_t key) {
    TimeTableEntry * entry = table[hash(key)].head;
    while(entry) {
        if (entry->key == key) break;
        entry = entry->next;
    }
    return entry;

}

void TimeTable::init(uint64_t thd_id, uint64_t key) {
    uint64_t idx = hash(key);
    uint64_t mtx_wait_starttime = get_sys_clock();
    pthread_mutex_lock(&table[idx].mtx);
    INC_STATS(thd_id,mtx[34],get_sys_clock() - mtx_wait_starttime);
    TimeTableEntry* entry = find(key);
    if(!entry) {
        DEBUG_M("TimeTable::init entry alloc\n");
        entry = (TimeTableEntry*) mem_allocator.alloc(sizeof(TimeTableEntry));
        entry->init(key);
        LIST_PUT_TAIL(table[idx].head,table[idx].tail,entry);
    }
    pthread_mutex_unlock(&table[idx].mtx);
}

void TimeTable::release(uint64_t thd_id, uint64_t key) {
    uint64_t idx = hash(key);
    uint64_t mtx_wait_starttime = get_sys_clock();
    pthread_mutex_lock(&table[idx].mtx);
    INC_STATS(thd_id,mtx[35],get_sys_clock() - mtx_wait_starttime);
    TimeTableEntry* entry = find(key);
    if(entry) {
        LIST_REMOVE_HT(entry,table[idx].head,table[idx].tail);
        DEBUG_M("TimeTable::release entry free\n");
        mem_allocator.free(entry,sizeof(TimeTableEntry));
    }
    pthread_mutex_unlock(&table[idx].mtx);
}

uint64_t TimeTable::get_lower(uint64_t thd_id, uint64_t key) {
    uint64_t idx = hash(key);
    uint64_t value = 0;
    uint64_t mtx_wait_starttime = get_sys_clock();
    pthread_mutex_lock(&table[idx].mtx);
    INC_STATS(thd_id,mtx[36],get_sys_clock() - mtx_wait_starttime);
    TimeTableEntry* entry = find(key);
    if(entry) {
        value = entry->lower;
    }
    pthread_mutex_unlock(&table[idx].mtx);
    return value;
}

uint64_t TimeTable::get_upper(uint64_t thd_id, uint64_t key) {
    uint64_t idx = hash(key);
    uint64_t value = UINT64_MAX;
    uint64_t mtx_wait_starttime = get_sys_clock();
    pthread_mutex_lock(&table[idx].mtx);
    INC_STATS(thd_id,mtx[37],get_sys_clock() - mtx_wait_starttime);
    TimeTableEntry* entry = find(key);
    if(entry) {
        value = entry->upper;
    }
    pthread_mutex_unlock(&table[idx].mtx);
    return value;
}


void TimeTable::set_lower(uint64_t thd_id, uint64_t key, uint64_t value) {
    uint64_t idx = hash(key);
    uint64_t mtx_wait_starttime = get_sys_clock();
    pthread_mutex_lock(&table[idx].mtx);
    INC_STATS(thd_id,mtx[38],get_sys_clock() - mtx_wait_starttime);
    TimeTableEntry* entry = find(key);
    if(entry) {
        entry->lower = value;
    }
    pthread_mutex_unlock(&table[idx].mtx);
}

void TimeTable::set_upper(uint64_t thd_id, uint64_t key, uint64_t value) {
    uint64_t idx = hash(key);
    uint64_t mtx_wait_starttime = get_sys_clock();
    pthread_mutex_lock(&table[idx].mtx);
    INC_STATS(thd_id,mtx[39],get_sys_clock() - mtx_wait_starttime);
    TimeTableEntry* entry = find(key);
    if(entry) {
        entry->upper = value;
    }
    pthread_mutex_unlock(&table[idx].mtx);
}

SILOState TimeTable::get_state(uint64_t thd_id, uint64_t key) {
    uint64_t idx = hash(key);
    SILOState state = SILO_ABORTED;
    uint64_t mtx_wait_starttime = get_sys_clock();
    pthread_mutex_lock(&table[idx].mtx);
    INC_STATS(thd_id,mtx[40],get_sys_clock() - mtx_wait_starttime);
    TimeTableEntry* entry = find(key);
    if(entry) {
        state = entry->state;
    }
    pthread_mutex_unlock(&table[idx].mtx);
    return state;
}

void TimeTable::set_state(uint64_t thd_id, uint64_t key, SILOState value) {
    uint64_t idx = hash(key);
    uint64_t mtx_wait_starttime = get_sys_clock();
    pthread_mutex_lock(&table[idx].mtx);
    INC_STATS(thd_id,mtx[41],get_sys_clock() - mtx_wait_starttime);
    TimeTableEntry* entry = find(key);
    if(entry) {
        entry->state = value;
    }
    pthread_mutex_unlock(&table[idx].mtx);
}

