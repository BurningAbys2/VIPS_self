/*
 * Copyright (c) 1999-2008 Mark D. Hill and David A. Wood
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <iomanip>

#include "arch/x86/ldstflags.hh"
#include "base/misc.hh"
#include "base/str.hh"
#include "cpu/testers/rubytest/RubyTester.hh"
#include "debug/MemoryAccess.hh"
#include "debug/ProtocolTrace.hh"
#include "debug/RubySequencer.hh"
#include "debug/RubySynStats.hh"
#include "debug/RubyStats.hh"
#include "debug/RubyEvent.hh"
#include "mem/protocol/PrefetchBit.hh"
#include "mem/protocol/RubyAccessMode.hh"
#include "mem/ruby/common/Global.hh"
#include "mem/ruby/profiler/Profiler.hh"
#include "mem/ruby/slicc_interface/RubyRequest.hh"
#include "mem/ruby/system/Sequencer.hh"
#include "mem/ruby/system/System.hh"
#include "mem/packet.hh"
#include "sim/system.hh"

using namespace std;

Sequencer *
RubySequencerParams::create()
{
    return new Sequencer(this);
}

Sequencer::Sequencer(const Params *p)
    : RubyPort(p), m_IncompleteTimes(MachineType_NUM), deadlockCheckEvent(this),delaywritethrougEvent(this)
{
    m_outstanding_count = 0;
    m_ShareAddress_num = 0;
	m_PrivateAddress_num = 0;
	m_PTEAddress_num = 0;
	m_lockWrite_ptr = NULL;
	flushpkt = NULL;
	m_triggerpte_ptr = NULL;
	m_third_type = RubyRequestType_NULL;
    m_instCache_ptr = p->icache;
    m_dataCache_ptr = p->dcache;
	m_pagetablebuffer = p->pagetable;
    m_max_outstanding_requests = p->max_outstanding_requests;
    m_deadlock_threshold = p->deadlock_threshold;
	triggerflush = false;

    assert(m_max_outstanding_requests > 0);
    assert(m_deadlock_threshold > 0);
    assert(m_instCache_ptr != NULL);
    assert(m_dataCache_ptr != NULL);

    m_usingNetworkTester = p->using_network_tester;
}

Sequencer::~Sequencer()
{
}

void
Sequencer::wakeup()
{
    assert(getDrainState() != Drainable::Draining);

    // Check for deadlock of any of the requests
    Cycles current_time = curCycle();

    // Check across all outstanding requests
    int total_outstanding = 0;

    RequestTable::iterator read = m_readRequestTable.begin();
    RequestTable::iterator read_end = m_readRequestTable.end();
    for (; read != read_end; ++read) {
        SequencerRequest* request = read->second;
        if (current_time - request->issue_time < m_deadlock_threshold)
            continue;

        panic("Possible Deadlock detected. Aborting!\n"
             "version: %d request.paddr: 0x%x m_readRequestTable: %d "
             "current time: %u issue_time: %d difference: %d\n", m_version,
             Address(request->pkt->getAddr()), m_readRequestTable.size(),
              current_time * clockPeriod(), request->issue_time * clockPeriod(),
              (current_time * clockPeriod()) - (request->issue_time * clockPeriod()));
    }

    RequestTable::iterator write = m_writeRequestTable.begin();
    RequestTable::iterator write_end = m_writeRequestTable.end();
    for (; write != write_end; ++write) {
        SequencerRequest* request = write->second;
        if (current_time - request->issue_time < m_deadlock_threshold)
            continue;

        panic("Possible Deadlock detected. Aborting!\n"
             "version: %d request.paddr: 0x%x m_writeRequestTable: %d "
             "current time: %u issue_time: %d difference: %d\n", m_version,
             Address(request->pkt->getAddr()), m_writeRequestTable.size(),
              current_time * clockPeriod(), request->issue_time * clockPeriod(),
              (current_time * clockPeriod()) - (request->issue_time * clockPeriod()));
    }

    total_outstanding += m_writeRequestTable.size();
    total_outstanding += m_readRequestTable.size();
	
    assert(m_outstanding_count == total_outstanding);
    if (m_outstanding_count > 0) {
        // If there are still outstanding requests, keep checking
        schedule(deadlockCheckEvent, clockEdge(m_deadlock_threshold));
    }
}
//@hxm***********************************************************************************************************
void
Sequencer::delaywritethrough()
{
	//issue write through
  if(!m_delaywritethrough.empty())
  {
  	if((m_delaywritethrough.begin()->first+1000) <= (uint64_t)curCycle())
  	 {
		 uint64_t delayaddress = m_delaywritethrough.begin()->second; 
		 std::shared_ptr<RubyRequest> msg = std::make_shared<RubyRequest>(clockEdge(), delayaddress, nullptr, 0, 0, RubyRequestType_ST_Unblock,RubyAccessMode_Supervisor, nullptr);   
		 Cycles latency(0);  // initialzed to an null value
		 latency = m_dataCache_ptr->getLatency();
		 m_mandatory_q_ptr->enqueue(msg, latency);	
		 m_delaywritethrough.erase(m_delaywritethrough.begin());
		 DPRINTF(RubyEvent, "delaywritethrough address:%#016x.\n", delayaddress);
	 }
	 if(!m_delaywritethrough.empty()){
	 	uint64_t delaycycle = m_delaywritethrough.begin()->first;
	 	schedule(delaywritethrougEvent,clockEdge(Cycles(delaycycle+1000)-curCycle()));
	}
 }
}

void 
Sequencer::startdelaywritethrough(const Address& address){
	assert(address == line_address(address));
	uint64_t delayaddress = address.getAddress();
	if(m_delaywritethrough.find((uint64_t)curCycle()) != m_delaywritethrough.end()) assert(false);
    m_delaywritethrough.insert(map<uint64_t,uint64_t>::value_type((uint64_t)curCycle(),delayaddress));
	if(!delaywritethrougEvent.scheduled()) schedule(delaywritethrougEvent,clockEdge(Cycles(1000)));
}

void
Sequencer::DelayWTRemove(const Address& address){
	assert(address == line_address(address));
	uint64_t delayaddress = address.getAddress();
	std::map<uint64_t,uint64_t>::iterator iter;
	for(iter = m_delaywritethrough.begin();iter != m_delaywritethrough.end();iter++){
		if(iter->second == delayaddress) m_delaywritethrough.erase(iter);
	}
}

void
Sequencer::checkL1cacheAddress(const Address& address,DataBlock& dblk,const MachineID& Machinenum){
	m_pagetablebuffer->checkcacheAddress(address,dblk,Machinenum);
}

bool 
Sequencer::checkDMAAddress(const Address& address){
   bool flags = m_pagetablebuffer->checkAddress(address);
   if(flags) DPRINTF(RubyEvent, "in sequence.cc checkDMA private address:%#016x.\n", address.getAddress());
   else DPRINTF(RubyEvent, "in sequence.cc checkDMA share address:%#016x.\n", address.getAddress()); 
   return flags;
}


//@hxm***********************************************************************************************************

void Sequencer::resetStats()
{
    m_latencyHist.reset();
    m_hitLatencyHist.reset();
    m_missLatencyHist.reset();
    for (int i = 0; i < RubyRequestType_NUM; i++) {
        m_typeLatencyHist[i]->reset();
        m_hitTypeLatencyHist[i]->reset();
        m_missTypeLatencyHist[i]->reset();
        for (int j = 0; j < MachineType_NUM; j++) {
            m_hitTypeMachLatencyHist[i][j]->reset();
            m_missTypeMachLatencyHist[i][j]->reset();
        }
    }

    for (int i = 0; i < MachineType_NUM; i++) {
        m_missMachLatencyHist[i]->reset();
        m_hitMachLatencyHist[i]->reset();

        m_IssueToInitialDelayHist[i]->reset();
        m_InitialToForwardDelayHist[i]->reset();
        m_ForwardToFirstResponseDelayHist[i]->reset();
        m_FirstResponseToCompletionDelayHist[i]->reset();

        m_IncompleteTimes[i] = 0;
    }
}

void
Sequencer::printProgress(ostream& out) const
{
#if 0
    int total_demand = 0;
    out << "Sequencer Stats Version " << m_version << endl;
    out << "Current time = " << g_system_ptr->getTime() << endl;
    out << "---------------" << endl;
    out << "outstanding requests" << endl;

    out << "proc " << m_Read
        << " version Requests = " << m_readRequestTable.size() << endl;

    // print the request table
    RequestTable::iterator read = m_readRequestTable.begin();
    RequestTable::iterator read_end = m_readRequestTable.end();
    for (; read != read_end; ++read) {
        SequencerRequest* request = read->second;
        out << "\tRequest[ " << i << " ] = " << request->type
            << " Address " << rkeys[i]
            << " Posted " << request->issue_time
            << " PF " << PrefetchBit_No << endl;
        total_demand++;
    }

    out << "proc " << m_version
        << " Write Requests = " << m_writeRequestTable.size << endl;

    // print the request table
    RequestTable::iterator write = m_writeRequestTable.begin();
    RequestTable::iterator write_end = m_writeRequestTable.end();
    for (; write != write_end; ++write) {
        SequencerRequest* request = write->second;
        out << "\tRequest[ " << i << " ] = " << request.getType()
            << " Address " << wkeys[i]
            << " Posted " << request.getTime()
            << " PF " << request.getPrefetch() << endl;
        if (request.getPrefetch() == PrefetchBit_No) {
            total_demand++;
        }
    }

    out << endl;

    out << "Total Number Outstanding: " << m_outstanding_count << endl
        << "Total Number Demand     : " << total_demand << endl
        << "Total Number Prefetches : " << m_outstanding_count - total_demand
        << endl << endl << endl;
#endif
}

// Insert the request on the correct request table.  Return true if
// the entry was already present.
RequestStatus
Sequencer::insertRequest(PacketPtr pkt, RubyRequestType request_type)
{
    assert(m_outstanding_count ==
        (m_writeRequestTable.size() + m_readRequestTable.size()));

    // See if we should schedule a deadlock check
    if (!deadlockCheckEvent.scheduled() &&
        getDrainState() != Drainable::Draining) {
        schedule(deadlockCheckEvent, clockEdge(m_deadlock_threshold));
    }

    Address line_addr(pkt->getAddr());
    line_addr.makeLineAddress();
    // Create a default entry, mapping the address to NULL, the cast is
    // there to make gcc 4.4 happy
    RequestTable::value_type default_entry(line_addr,
                                           (SequencerRequest*) NULL);

    if ((request_type == RubyRequestType_ST) ||
		(request_type == RubyRequestType_ST_P) ||
        (request_type == RubyRequestType_RMW_Read) ||
        (request_type == RubyRequestType_RMW_Write) ||
        (request_type == RubyRequestType_Load_Linked) ||
        (request_type == RubyRequestType_Store_Conditional) ||
        (request_type == RubyRequestType_Locked_RMW_Read) ||
        (request_type == RubyRequestType_Locked_RMW_Write) ||
        (request_type == RubyRequestType_FLUSH)) {

        // Check if there is any outstanding read request for the same
        // cache line.
        if (m_readRequestTable.count(line_addr) > 0) {
            m_store_waiting_on_load++;
            return RequestStatus_Aliased;
        }

        pair<RequestTable::iterator, bool> r =
            m_writeRequestTable.insert(default_entry);
        if (r.second) {
            RequestTable::iterator i = r.first;
            i->second = new SequencerRequest(pkt, request_type, curCycle());
            m_outstanding_count++;
        } else {
          // There is an outstanding write request for the cache line
          m_store_waiting_on_store++;
          return RequestStatus_Aliased;
        }
    } else {
        // Check if there is any outstanding write request for the same
        // cache line.
        if (m_writeRequestTable.count(line_addr) > 0) {
            m_load_waiting_on_store++;
            return RequestStatus_Aliased;
        }

        pair<RequestTable::iterator, bool> r =
            m_readRequestTable.insert(default_entry);

        if (r.second) {
            RequestTable::iterator i = r.first;
            i->second = new SequencerRequest(pkt, request_type, curCycle());
            m_outstanding_count++;
        } else {
            // There is an outstanding read request for the cache line
            m_load_waiting_on_load++;
            return RequestStatus_Aliased;
        }
    }

    m_outstandReqHist.sample(m_outstanding_count);
    assert(m_outstanding_count ==
        (m_writeRequestTable.size() + m_readRequestTable.size()));

    return RequestStatus_Ready;
}

void
Sequencer::markRemoved()
{
    m_outstanding_count--;
    assert(m_outstanding_count ==
           m_writeRequestTable.size() + m_readRequestTable.size());
}

void
Sequencer::removeRequest(SequencerRequest* srequest)
{
    assert(m_outstanding_count ==
           m_writeRequestTable.size() + m_readRequestTable.size());

    Address line_addr(srequest->pkt->getAddr());
    line_addr.makeLineAddress();
    if ((srequest->m_type == RubyRequestType_ST) ||
        (srequest->m_type == RubyRequestType_ST_P) || 
        (srequest->m_type == RubyRequestType_RMW_Read) ||
        (srequest->m_type == RubyRequestType_RMW_Write) ||
        (srequest->m_type == RubyRequestType_Load_Linked) ||
        (srequest->m_type == RubyRequestType_Store_Conditional) ||
        (srequest->m_type == RubyRequestType_Locked_RMW_Read) ||
        (srequest->m_type == RubyRequestType_Locked_RMW_Write)) {
        m_writeRequestTable.erase(line_addr);
    } else {
        m_readRequestTable.erase(line_addr);
    }

    markRemoved();
}

void
Sequencer::invalidateSC(const Address& address)
{
    RequestTable::iterator i = m_writeRequestTable.find(address);
    if (i != m_writeRequestTable.end()) {
        SequencerRequest* request = i->second;
        // The controller has lost the coherence permissions, hence the lock
        // on the cache line maintained by the cache should be cleared.
        if (request->m_type == RubyRequestType_Store_Conditional) {
            m_dataCache_ptr->clearLocked(address);
        }
    }
}

bool
Sequencer::handleLlsc(const Address& address, SequencerRequest* request)
{
    //
    // The success flag indicates whether the LLSC operation was successful.
    // LL ops will always succeed, but SC may fail if the cache line is no
    // longer locked.
    //
    bool success = true;
    if (request->m_type == RubyRequestType_Store_Conditional) {
        if (!m_dataCache_ptr->isLocked(address, m_version)) {
            //
            // For failed SC requests, indicate the failure to the cpu by
            // setting the extra data to zero.
            //
            request->pkt->req->setExtraData(0);
            success = false;
        } else {
            //
            // For successful SC requests, indicate the success to the cpu by
            // setting the extra data to one.  
            //
            request->pkt->req->setExtraData(1);
        }
        //
        // Independent of success, all SC operations must clear the lock
        //
        m_dataCache_ptr->clearLocked(address);
    } else if (request->m_type == RubyRequestType_Load_Linked) {
        //
        // Note: To fully follow Alpha LLSC semantics, should the LL clear any
        // previously locked cache lines?
        //
        m_dataCache_ptr->setLocked(address, m_version);
    } else if ((m_dataCache_ptr->isTagPresent(address)) &&
               (m_dataCache_ptr->isLocked(address, m_version))) {
        //
        // Normal writes should clear the locked address
        //
        m_dataCache_ptr->clearLocked(address);
    }
    return success;
}

void
Sequencer::recordMissLatency(const Cycles cycles, const RubyRequestType type,
                             const MachineType respondingMach,
                             bool isExternalHit, Cycles issuedTime,
                             Cycles initialRequestTime,
                             Cycles forwardRequestTime,
                             Cycles firstResponseTime, Cycles completionTime)
{
    m_latencyHist.sample(cycles);
    m_typeLatencyHist[type]->sample(cycles);

    if (isExternalHit) {
        m_missLatencyHist.sample(cycles);
        m_missTypeLatencyHist[type]->sample(cycles);

        if (respondingMach != MachineType_NUM) {
            m_missMachLatencyHist[respondingMach]->sample(cycles);
            m_missTypeMachLatencyHist[type][respondingMach]->sample(cycles);

            if ((issuedTime <= initialRequestTime) &&
                (initialRequestTime <= forwardRequestTime) &&
                (forwardRequestTime <= firstResponseTime) &&
                (firstResponseTime <= completionTime)) {

                m_IssueToInitialDelayHist[respondingMach]->sample(
                    initialRequestTime - issuedTime);
                m_InitialToForwardDelayHist[respondingMach]->sample(
                    forwardRequestTime - initialRequestTime);
                m_ForwardToFirstResponseDelayHist[respondingMach]->sample(
                    firstResponseTime - forwardRequestTime);
                m_FirstResponseToCompletionDelayHist[respondingMach]->sample(
                    completionTime - firstResponseTime);
            } else {
                m_IncompleteTimes[respondingMach]++;
            }
        }
    } else {
        m_hitLatencyHist.sample(cycles);
        m_hitTypeLatencyHist[type]->sample(cycles);

        if (respondingMach != MachineType_NUM) {
            m_hitMachLatencyHist[respondingMach]->sample(cycles);
            m_hitTypeMachLatencyHist[type][respondingMach]->sample(cycles);
        }
    }
}

void
Sequencer::writeCallback(const Address& address, DataBlock& data,
                         const bool externalHit, const MachineType mach,
                         const Cycles initialRequestTime,
                         const Cycles forwardRequestTime,
                         const Cycles firstResponseTime)
{
    assert(address == line_address(address));
    assert(m_writeRequestTable.count(line_address(address)));

    RequestTable::iterator i = m_writeRequestTable.find(address);
    assert(i != m_writeRequestTable.end());
    SequencerRequest* request = i->second;

    m_writeRequestTable.erase(i);
    markRemoved();

    assert((request->m_type == RubyRequestType_ST) ||
           (request->m_type == RubyRequestType_ST_P) ||
           (request->m_type == RubyRequestType_ATOMIC) ||
           (request->m_type == RubyRequestType_RMW_Read) ||
           (request->m_type == RubyRequestType_RMW_Write) ||
           (request->m_type == RubyRequestType_Load_Linked) ||
           (request->m_type == RubyRequestType_Store_Conditional) ||
           (request->m_type == RubyRequestType_Locked_RMW_Read) ||
           (request->m_type == RubyRequestType_Locked_RMW_Write) ||
           (request->m_type == RubyRequestType_FLUSH));

    // For Alpha, properly handle LL, SC, and write requests with respect to
    // locked cache blocks.
    // Not valid for Network_test protocl
    bool success = true;
    if(!m_usingNetworkTester)
        success = handleLlsc(address, request);
    hitCallback(request, data, success, mach, externalHit,
                initialRequestTime, forwardRequestTime, firstResponseTime);
}

void
Sequencer::readCallback(const Address& address, DataBlock& data,
                        bool externalHit, const MachineType mach,
                        Cycles initialRequestTime,
                        Cycles forwardRequestTime,
                        Cycles firstResponseTime)
{
    assert(address == line_address(address));
    assert(m_readRequestTable.count(line_address(address)));

    RequestTable::iterator i = m_readRequestTable.find(address);
    assert(i != m_readRequestTable.end());
    SequencerRequest* request = i->second;

    m_readRequestTable.erase(i);
    markRemoved();

    assert((request->m_type == RubyRequestType_LD) ||
  	   	   (request->m_type == RubyRequestType_LD_P) ||
           (request->m_type == RubyRequestType_IFETCH_P)||
           (request->m_type == RubyRequestType_IFETCH));

    hitCallback(request, data, true, mach, externalHit,
                initialRequestTime, forwardRequestTime, firstResponseTime);
}

void
Sequencer::hitCallback(SequencerRequest* srequest, DataBlock& data,
                       bool llscSuccess,
                       const MachineType mach, const bool externalHit,
                       const Cycles initialRequestTime,
                       const Cycles forwardRequestTime,
                       const Cycles firstResponseTime)
{
    PacketPtr pkt = srequest->pkt;
    Address request_address(pkt->getAddr());
    Address request_line_address(pkt->getAddr());
    request_line_address.makeLineAddress();
    RubyRequestType type = srequest->m_type;
    Cycles issued_time = srequest->issue_time;

    // Set this cache entry to the most recently used
    if (type == RubyRequestType_IFETCH || RubyRequestType_IFETCH_P) {
        m_instCache_ptr->setMRU(request_line_address);
    } else {
        m_dataCache_ptr->setMRU(request_line_address);
    }

    assert(curCycle() >= issued_time);
    Cycles total_latency = curCycle() - issued_time;

    // Profile the latency for all demand accesses.
    recordMissLatency(total_latency, type, mach, externalHit, issued_time,
                      initialRequestTime, forwardRequestTime,
                      firstResponseTime, curCycle());

    DPRINTFR(ProtocolTrace, "%15s %3s %10s%20s %6s>%-6s %s %d cycles\n",
             curTick(), m_version, "Seq",
             llscSuccess ? "Done" : "SC_Failed", "", "",
             request_address, total_latency);

    // update the data unless it is a non-data-carrying flush
    if (RubySystem::getWarmupEnabled()) {
        data.setData(pkt->getConstPtr<uint8_t>(),
                     request_address.getOffset(), pkt->getSize());
    } else if (!pkt->isFlush()) {
        if ((type == RubyRequestType_LD) ||
       	    (type == RubyRequestType_LD_P) ||
            (type == RubyRequestType_IFETCH) ||
            (type == RubyRequestType_IFETCH_P) ||
            (type == RubyRequestType_RMW_Read) ||
            (type == RubyRequestType_Locked_RMW_Read) ||
            (type == RubyRequestType_Load_Linked)) {
            memcpy(pkt->getPtr<uint8_t>(),
                   data.getData(request_address.getOffset(), pkt->getSize()),
                   pkt->getSize());
        } else {
            data.setData(pkt->getConstPtr<uint8_t>(),
                         request_address.getOffset(), pkt->getSize());
			data.setflag(pkt->getConstPtr<uint8_t>(),
                         request_address.getOffset(), pkt->getSize());
        }
    }

    // If using the RubyTester, update the RubyTester sender state's
    // subBlock with the recieved data.  The tester will later access
    // this state.
    if (m_usingRubyTester) {
        DPRINTF(RubySequencer, "hitCallback %s 0x%x using RubyTester\n",
                pkt->cmdString(), pkt->getAddr());
        RubyTester::SenderState* testerSenderState =
            pkt->findNextSenderState<RubyTester::SenderState>();
        assert(testerSenderState);
        testerSenderState->subBlock.mergeFrom(data);
    }

    delete srequest;
    if (RubySystem::getWarmupEnabled()) {
        assert(pkt->req);
        delete pkt->req;
        delete pkt;
        g_system_ptr->m_cache_recorder->enqueueNextFetchRequest();
    } else if (RubySystem::getCooldownEnabled()) {
        delete pkt;
        g_system_ptr->m_cache_recorder->enqueueNextFlushRequest();
    } else {
        ruby_hit_callback(pkt);
    }
	
}

bool
Sequencer::empty() const
{
    return m_writeRequestTable.empty() && m_readRequestTable.empty();
}

RequestStatus
Sequencer::makeRequest(PacketPtr pkt)
{
    if (m_outstanding_count >= m_max_outstanding_requests) {
        return RequestStatus_BufferFull;
    }
    RubyRequestType primary_type = RubyRequestType_NULL;
    RubyRequestType secondary_type = RubyRequestType_NULL;
    //@hxm*********************************************************
    //using for handle the three level page access
    DPRINTFR(ProtocolTrace, "%3s enter makeRequest\n",m_version);
	if(pkt->req->getpdp())//rejudge share or private
	{
      //check this 4kb page or add into the page entry;
      if(m_pagetablebuffer->checkAddress(pkt,m_version)){
          if(pkt->trigger) return RequestStatus_BufferFull;
		  pkt->req->setpageprivate(true);//private
	  }else pkt->req->setpageprivate(false);//share
	}
	//@hxm*********************************************************
    //if 2M page then look page table for private or share 
    if(pkt->req->getpte()){//pagetalbe access should be token care of 
		if(pkt->isWrite()){
		    primary_type = RubyRequestType_Locked_RMW_Write;
			secondary_type = RubyRequestType_ST_PTE;
			m_PrivateAddress.erase(pkt->getAddr());
			m_ShareAddress.erase(pkt->getAddr());
			m_PTEAddress.erase(pkt->getAddr());
			//the last one
			if(pkt->req->getpte()==4 && m_PTEAddress.size()>0){
			//if(m_PTEAddress.size()>0){
				do{
					uint64_t PTEAddr = getPTEAddress();
					m_ShareAddress.erase(PTEAddr);
					m_PrivateAddress.erase(PTEAddr);
					std::shared_ptr<RubyRequest> msg = std::make_shared<RubyRequest>(clockEdge(), PTEAddr,
				                  nullptr, 0, 0, RubyRequestType_ST_Unblock,
				                  RubyAccessMode_Supervisor, nullptr);	
					DPRINTFR(ProtocolTrace, "%15s %3s %10s%20s %6s>%-6s %s %s\n",
				    		  curTick(), m_version, "Seq", "Begin", "", "",
				    		  msg->getPhysicalAddress(),RubyRequestType_to_string(RubyRequestType_ST_Unblock));
					Cycles latency(0);  // initialzed to an null value
					latency = m_dataCache_ptr->getLatency();
					m_mandatory_q_ptr->enqueue(msg, latency);	
					m_PTEAddress_num++;
			  }while(m_PTEAddress.size()>0);
			  m_lockWrite_ptr = pkt;
			  m_primary_type = primary_type;
			  m_third_type= secondary_type;
			  assert(m_lockWrite_ptr!= NULL);
			  return RequestStatus_Issued;
			}
		}else{
			primary_type =  RubyRequestType_Locked_RMW_Read;
			int ptestate = pkt->req->getpte();	
			if(ptestate == 2) 
		    	secondary_type = RubyRequestType_LD_PDP;//long PDP when is the last pkt to read the page enty,we should treat differently
		    else if(ptestate == 3)
				secondary_type = RubyRequestType_LD_PTE;//longPTE
			else secondary_type = RubyRequestType_LD_PD;//longPML4 or longPD
			m_PTEAddress.insert(pkt->getAddr());
		}
	}else if (pkt->isLLSC()) {
        //
        // Alpha LL/SC instructions need to be handled carefully by the cache
        // coherence protocol to ensure they follow the proper semantics. In
        // particular, by identifying the operations as atomic, the protocol
        // should understand that migratory sharing optimizations should not
        // be performed (i.e. a load between the LL and SC should not steal
        // away exclusive permission).
        //
        if (pkt->isWrite()) {
            DPRINTF(RubySequencer, "Issuing SC\n");		
            primary_type = RubyRequestType_Store_Conditional;
        } else {
            DPRINTF(RubySequencer, "Issuing LL\n");
            assert(pkt->isRead());
            primary_type = RubyRequestType_Load_Linked;
        }
		 secondary_type = RubyRequestType_ATOMIC;
	     DPRINTF(RubySequencer,"in sequencer isLLSC pkt->req->getpageprivate()=%d\n",pkt->req->getpageprivate());
    } else if (pkt->req->isLockedRMW()) {
        //
        // x86 locked instructions are translated to store cache coherence
        // requests because these requests should always be treated as read
        // exclusive operations and should leverage any migratory sharing
        // optimization built into the protocol.
        //@hxm when the req is LockedRMW we should lock the memory locate so modify here
        //there we assume every RMW is share and all the operation keep exclusive
        if (pkt->isWrite()) {
	            DPRINTF(RubySequencer, "Issuing Locked RMW Write\n");
	            primary_type = RubyRequestType_Locked_RMW_Write;
				if(pkt->req->getpageprivate()) {
					if(pkt->getAddr()>=0x6000 && pkt->getAddr() <0x7000) {
						secondary_type = RubyRequestType_ST;
						DPRINTF(RubySequencer, "issuing RubyRequestType_ST\n");
					}
					else {
						secondary_type = RubyRequestType_ST_P;
						DPRINTF(RubySequencer, "issuing RubyRequestType_ST_P\n");
					}
				}
				else  secondary_type = RubyRequestType_Locked_RMW_Write;
        } else {
            DPRINTF(RubySequencer, "Issuing Locked RMW Read\n");
            assert(pkt->isRead());
            primary_type = secondary_type = RubyRequestType_Locked_RMW_Read;	
			if(pkt->req->getpageprivate()) secondary_type = RubyRequestType_ST_P;
			else {
					secondary_type = RubyRequestType_Locked_RMW_Read;
			}
        }
    } else {
	    if (pkt->isRead()) {
		    if (pkt->req->isInstFetch()) {
			    if(pkt->req->getpageprivate()){
				    primary_type = secondary_type = RubyRequestType_IFETCH_P;
				    DPRINTF(RubySequencer, "Issuing RubyRequestType_IFETCH_P\n");
			    }else {
				    primary_type = secondary_type = RubyRequestType_IFETCH;
				    DPRINTF(RubySequencer, "Issuing RubyRequestType_IFETCH\n");
			    }
		    } else {
			    bool storeCheck = false;
			    // only X86 need the store check
			    if (system->getArch() == Arch::X86ISA) {
				    uint32_t flags = pkt->req->getFlags();
				    storeCheck = flags &
					    (X86ISA::StoreCheck << X86ISA::FlagShift);
			    }
			    if (storeCheck || pkt->req->getFalseldst()) {
				    primary_type = RubyRequestType_RMW_Read;
				    DPRINTF(RubySequencer, "RubyRequestType_RMW_Read\n");
				    DPRINTF(RubySequencer, "in sequencer storecheck pkt->req->getpageprivate()=%d\n",pkt->req->getpageprivate());
				    if(pkt->req->getpageprivate()) secondary_type = RubyRequestType_ST_P;
                                    else secondary_type = RubyRequestType_ST_T;
			    } else {
				    if(pkt->req->getpageprivate()){
					    primary_type = secondary_type = RubyRequestType_LD_P;
					    DPRINTF(RubySequencer, "issuing RubyRequestType_LD_P\n");
				    }
				    else {
					    primary_type = secondary_type = RubyRequestType_LD;
					    DPRINTF(RubySequencer, "issuing RubyRequestType_LD\n");
				    }
			    }
            }
        } else if (pkt->isWrite()) {
		//
		// Note: M5 packets do not differentiate ST from RMW_Write
		if(pkt ->getAddr()==0x6051c0) DPRINTF(RubySequencer, "@hxm get right synchronization piont.\n");
		if(pkt->req->getpageprivate()){
			if(pkt->getAddr()>=0x6000 && pkt->getAddr() <= 0x7000) {
				primary_type = secondary_type = RubyRequestType_ST;
				DPRINTF(RubySequencer, "issuing RubyRequestType_ST\n");
			}
			else {
				primary_type = secondary_type = RubyRequestType_ST_P;
			    DPRINTF(RubySequencer, "issuing RubyRequestType_ST_P\n");
			}
		}
		else {
			primary_type = secondary_type = RubyRequestType_ST;
			DPRINTF(RubySequencer, "issuing RubyRequestType_ST\n");
		}
	}else if(pkt->isFlush()){
			//transiton beteewn share and private
			if(m_PrivateAddress.size()>0){
			    do{
					//std::cout<<"@hxm m_PrivateAddress_num ="<<m_PrivateAddress_num<<std::endl;
					uint64_t privetAddr = getPrivateAddress();
					//if(privateaddress=="0x203fc0") std::cout<<"enter isFlush\n"<<std::endl;
					//std::cout<<"@hxm synchronziation paddr=0x"<<std::hex<<privetAddr<<std::endl;
					std::shared_ptr<RubyRequest> msg = std::make_shared<RubyRequest>(clockEdge(), privetAddr,
			                  nullptr, 0, 0, RubyRequestType_FLUSH_Transition,
			                  RubyAccessMode_Supervisor, nullptr);	
					 DPRINTFR(ProtocolTrace, "%15s %3s %10s%20s %6s>%-6s %s %s\n",
			    				  curTick(), m_version, "Seq", "Begin", "", "",
			    				  msg->getPhysicalAddress(),RubyRequestType_to_string(RubyRequestType_FLUSH_Transition));
					Cycles latency(0);  // initialzed to an null value
					latency = m_dataCache_ptr->getLatency();
					m_mandatory_q_ptr->enqueue(msg, latency);
					m_PrivateAddress_num ++;					
				  }while(m_PrivateAddress.size()>0);
				  flushpkt = pkt;//for flush packet to callback 
//				  std::cout<<"@hxm m_PrivateAddress_num ="<<m_PrivateAddress_num<<std::endl;
		      }else{
					std::shared_ptr<RubyRequest> msg = std::make_shared<RubyRequest>(clockEdge(), pkt->getAddr(),
			                  nullptr, 0, 0, RubyRequestType_FLUSH_I,
			                  RubyAccessMode_Supervisor, nullptr);  
					Cycles latency(0);  // initialzed to an null value
					latency = m_dataCache_ptr->getLatency();
					m_mandatory_q_ptr->enqueue(msg, latency);
					flushpkt = pkt;//for flush packet to callback 
					//std::cout<<"issue flush but don't flush anything."<<std::endl;
		      }
		      return RequestStatus_Issued;		
	}else {
		panic("Unsupported ruby packet type\n");
	}
   }
   if(triggerflush){
		triggerflush = false;
		if(m_ShareAddress.size()>0){
					while(!ShareIsempty()){
							uint64_t shareaddress = getShareAddress();
			                DPRINTF(RubySequencer, "@hxm synchronziation paddr=%#x.\n",shareaddress);
							//std::cout<<"@hxm synchronziation paddr=0x"<<std::hex<<shareaddress<<std::endl;	
							std::shared_ptr<RubyRequest> msg =
       								 std::make_shared<RubyRequest>(clockEdge(), shareaddress,
                                      nullptr, 0, 0, RubyRequestType_FLUSH,
                                      RubyAccessMode_Supervisor, nullptr);	
						    DPRINTFR(ProtocolTrace, "%15s %3s %10s%20s %6s>%-6s %s %s\n",
          								  curTick(), m_version, "Seq", "Begin", "", "",
          								  msg->getPhysicalAddress(),
          								  RubyRequestType_to_string(RubyRequestType_FLUSH));
							Cycles latency(0);  // initialzed to an null value
							latency = m_dataCache_ptr->getLatency();
							m_mandatory_q_ptr->enqueue(msg, latency);
							m_ShareAddress_num ++;					
						  }
					//std::cout<<"@hxm m_ShareAddress_num ="<<m_ShareAddress_num<<std::endl;
					m_lockWrite_ptr = pkt;
					m_primary_type = primary_type;
					m_third_type= secondary_type;
					assert(m_lockWrite_ptr!= NULL);
					return RequestStatus_Issued;
		}	
		//std::cout<<"@hxm m_ShareAddress_num =0x"<<m_ShareAddress_num<<std::endl;
	}
    if(secondary_type == RubyRequestType_Locked_RMW_Read) DelayWTRemove(line_address(Address(pkt->getAddr())));
	if(secondary_type == RubyRequestType_Locked_RMW_Write) triggerflush = true;
    RequestStatus status = insertRequest(pkt, primary_type);
	if (status != RequestStatus_Ready)   return status;
    issueRequest(pkt, secondary_type);
    // TODO: issue hardware prefetches here
	return RequestStatus_Issued;
}

void
Sequencer::issueRequest(PacketPtr pkt, RubyRequestType secondary_type)
{
    assert(pkt != NULL);
    int proc_id = -1;
    if(pkt->req->hasContextId()) {
        proc_id = pkt->req->contextId();
    }

    // If valid, copy the pc to the ruby request
    Addr pc = 0;
    if (pkt->req->hasPC()) {
        pc = pkt->req->getPC();
    }

    // check if the packet has data as for example prefetch and flush
    // requests do not
    std::shared_ptr<RubyRequest> msg =
        std::make_shared<RubyRequest>(clockEdge(), pkt->getAddr(),
                                      pkt->isFlush() ?
                                      nullptr : pkt->getPtr<uint8_t>(),
                                      pkt->getSize(), pc, secondary_type,
                                      RubyAccessMode_Supervisor, pkt,
                                      PrefetchBit_No, proc_id);
	
    DPRINTFR(ProtocolTrace, "%15s %3s %10s%20s %6s>%-6s %s %s\n",
            curTick(), m_version, "Seq", "Begin", "", "",
            msg->getPhysicalAddress(),
            RubyRequestType_to_string(secondary_type));

    Cycles latency(0);  // initialzed to an null value

    if (secondary_type == RubyRequestType_IFETCH || secondary_type == RubyRequestType_IFETCH_P)
        latency = m_instCache_ptr->getLatency();
    else
        latency = m_dataCache_ptr->getLatency();

    // Send the message to the cache controller
    assert(latency > 0);

    assert(m_mandatory_q_ptr != NULL);
    m_mandatory_q_ptr->enqueue(msg, latency);	
}

template <class KEY, class VALUE>
std::ostream &
operator<<(ostream &out, const m5::hash_map<KEY, VALUE> &map)
{
    typename m5::hash_map<KEY, VALUE>::const_iterator i = map.begin();
    typename m5::hash_map<KEY, VALUE>::const_iterator end = map.end();

    out << "[";
    for (; i != end; ++i)
        out << " " << i->first << "=" << i->second;
    out << " ]";

    return out;
}

void
Sequencer::print(ostream& out) const
{
    out << "[Sequencer: " << m_version
        << ", outstanding requests: " << m_outstanding_count
        << ", read request table: " << m_readRequestTable
        << ", write request table: " << m_writeRequestTable
        << "]";
}

// this can be called from setState whenever coherence permissions are
// upgraded when invoked, coherence violations will be checked for the
// given block
void
Sequencer::checkCoherence(const Address& addr)
{
#ifdef CHECK_COHERENCE
    g_system_ptr->checkGlobalCoherenceInvariant(addr);
#endif
}

void
Sequencer::recordRequestType(SequencerRequestType requestType) {
    DPRINTF(RubyStats, "Recorded statistic: %s\n",
            SequencerRequestType_to_string(requestType));
}
void
Sequencer::evictionCallback(const Address& address)
{
    ruby_eviction_callback(address);
}
//@hxm***********************************************************
//recourd shared data invoted by cache control
void
Sequencer::reordShareAddress(const Address& address)
{
   assert(address == line_address(address));
   uint64_t shareaddress = address.getAddress();
   m_ShareAddress.insert(shareaddress);
}

void
Sequencer::reordPrivateAddress(const Address& address)
{
   //std::cout<<"enter reordShareAddress"<<std::endl;
   assert(address == line_address(address));
   uint64_t privateaddress = address.getAddress();
   m_PrivateAddress.insert(privateaddress);
}

//move shared data invoted by cache control
void
Sequencer::moveShareAddress(const Address& address)
{
   //std::cout<<"enter moveShareAddress"<<std::endl;
   assert(address == line_address(address));
   uint64_t shareaddress = address.getAddress();
   m_ShareAddress.erase(shareaddress);
   //if(shareaddress==0x203fc0) std::cout<<"enter moveShareAddress\n"<<std::endl;
   //std::cout<<"leave moveShareAddress"<<std::endl;
}

//move shared data invoted by cache control
void
Sequencer::movePrivateAddress(const Address& address)
{
   assert(address == line_address(address));
   uint64_t privateaddress = address.getAddress();
   m_PrivateAddress.erase(privateaddress);
   //if(privateaddress==0x203fc0) std::cout<<"enter movePrivateAddress\n"<<std::endl;
}

//before using this function please sure m_ShareAddress empty or not
uint64_t
Sequencer::getShareAddress()
{
   uint64_t shareaddress = *(m_ShareAddress.begin());
   m_ShareAddress.erase(m_ShareAddress.begin());
  // if(shareaddress==0x203fc0) std::cout<<"enter movePrivateAddress\n"<<std::endl;
   return shareaddress;

}

uint64_t
Sequencer::getPrivateAddress()
{
   //std::cout<<"enter getShareAddress"<<std::endl;
   uint64_t privateaddress = *(m_PrivateAddress.begin());
   m_PrivateAddress.erase(m_PrivateAddress.begin());
   //std::cout<<"leave getShareAddress"<<std::endl;
   //if(privateaddress==0x203fc0) std::cout<<"enter getPrivateAddress\n"<<std::endl;
   return privateaddress;

}

uint64_t
Sequencer::getPTEAddress()
{
   uint64_t pteaddress = *(m_PTEAddress.begin());
   m_PTEAddress.erase(m_PTEAddress.begin());
   return pteaddress;
}

//using for slicc issuing requset after synchroniziton
void 
Sequencer::makeLockWriteRequest() 
{
	assert(m_lockWrite_ptr != NULL);
	if(m_third_type == RubyRequestType_Locked_RMW_Write) 
		triggerflush = true;
	insertRequest(m_lockWrite_ptr, m_primary_type);
	issueRequest(m_lockWrite_ptr,m_third_type);
	m_ShareAddress_num = 0;
}

void 
Sequencer::flushCallback()
{ 
//	std::cout<<"in Sequencer.hh flush all private data"<<std::endl;
	ruby_hit_callback(flushpkt);
	m_PrivateAddress_num = 0;
}

void
Sequencer::PTECallback()
{
  //std::cout<<"in sequencer.hh PTE unblock all pte address."<<std::endl;
  insertRequest(m_lockWrite_ptr, m_primary_type);
  issueRequest(m_lockWrite_ptr,m_third_type);
  m_PTEAddress_num = 0;
}
//@hxm***********************************************************************

void
Sequencer::regStats()
{
    m_store_waiting_on_load
        .name(name() + ".store_waiting_on_load")
        .desc("Number of times a store aliased with a pending load")
        .flags(Stats::nozero);
    m_store_waiting_on_store
        .name(name() + ".store_waiting_on_store")
        .desc("Number of times a store aliased with a pending store")
        .flags(Stats::nozero);
    m_load_waiting_on_load
        .name(name() + ".load_waiting_on_load")
        .desc("Number of times a load aliased with a pending load")
        .flags(Stats::nozero);
    m_load_waiting_on_store
        .name(name() + ".load_waiting_on_store")
        .desc("Number of times a load aliased with a pending store")
        .flags(Stats::nozero);

    // These statistical variables are not for display.
    // The profiler will collate these across different
    // sequencers and display those collated statistics.
    m_outstandReqHist.init(10);
    m_latencyHist.init(10);
    m_hitLatencyHist.init(10);
    m_missLatencyHist.init(10);

    for (int i = 0; i < RubyRequestType_NUM; i++) {
        m_typeLatencyHist.push_back(new Stats::Histogram());
        m_typeLatencyHist[i]->init(10);

        m_hitTypeLatencyHist.push_back(new Stats::Histogram());
        m_hitTypeLatencyHist[i]->init(10);

        m_missTypeLatencyHist.push_back(new Stats::Histogram());
        m_missTypeLatencyHist[i]->init(10);
    }

    for (int i = 0; i < MachineType_NUM; i++) {
        m_hitMachLatencyHist.push_back(new Stats::Histogram());
        m_hitMachLatencyHist[i]->init(10);

        m_missMachLatencyHist.push_back(new Stats::Histogram());
        m_missMachLatencyHist[i]->init(10);

        m_IssueToInitialDelayHist.push_back(new Stats::Histogram());
        m_IssueToInitialDelayHist[i]->init(10);

        m_InitialToForwardDelayHist.push_back(new Stats::Histogram());
        m_InitialToForwardDelayHist[i]->init(10);

        m_ForwardToFirstResponseDelayHist.push_back(new Stats::Histogram());
        m_ForwardToFirstResponseDelayHist[i]->init(10);

        m_FirstResponseToCompletionDelayHist.push_back(new Stats::Histogram());
        m_FirstResponseToCompletionDelayHist[i]->init(10);
    }

    for (int i = 0; i < RubyRequestType_NUM; i++) {
        m_hitTypeMachLatencyHist.push_back(std::vector<Stats::Histogram *>());
        m_missTypeMachLatencyHist.push_back(std::vector<Stats::Histogram *>());

        for (int j = 0; j < MachineType_NUM; j++) {
            m_hitTypeMachLatencyHist[i].push_back(new Stats::Histogram());
            m_hitTypeMachLatencyHist[i][j]->init(10);

            m_missTypeMachLatencyHist[i].push_back(new Stats::Histogram());
            m_missTypeMachLatencyHist[i][j]->init(10);
        }
    }
}
