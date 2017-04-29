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

#ifndef __MEM_RUBY_SYSTEM_SEQUENCER_HH__
#define __MEM_RUBY_SYSTEM_SEQUENCER_HH__

#include <iostream>
#include <set>
#include <map>
#include <queue>

#include "base/hashmap.hh"
#include "mem/protocol/MachineType.hh"
#include "mem/protocol/RubyRequestType.hh"
#include "mem/protocol/SequencerRequestType.hh"
#include "mem/ruby/common/Address.hh"
#include "mem/ruby/structures/CacheMemory.hh"
#include "mem/ruby/structures/PageTableBuffer.hh"
#include "mem/ruby/system/RubyPort.hh"
#include "params/RubySequencer.hh"

struct SequencerRequest
{
    PacketPtr pkt;
    RubyRequestType m_type;
    Cycles issue_time;

    SequencerRequest(PacketPtr _pkt, RubyRequestType _m_type,
                     Cycles _issue_time)
        : pkt(_pkt), m_type(_m_type), issue_time(_issue_time)
    {}
};

std::ostream& operator<<(std::ostream& out, const SequencerRequest& obj);

class Sequencer : public RubyPort
{
  public:
    typedef RubySequencerParams Params;
    Sequencer(const Params *);
    ~Sequencer();

    // Public Methods
    void wakeup(); // Used only for deadlock detection
    void delaywritethrough();//used only for delay write-through
    void printProgress(std::ostream& out) const;
    void resetStats();
    void collateStats();
    void regStats();

    void writeCallback(const Address& address,
                       DataBlock& data,
                       const bool externalHit = false,
                       const MachineType mach = MachineType_NUM,
                       const Cycles initialRequestTime = Cycles(0),
                       const Cycles forwardRequestTime = Cycles(0),
                       const Cycles firstResponseTime = Cycles(0));

    void readCallback(const Address& address,
                      DataBlock& data,
                      const bool externalHit = false,
                      const MachineType mach = MachineType_NUM,
                      const Cycles initialRequestTime = Cycles(0),
                      const Cycles forwardRequestTime = Cycles(0),
                      const Cycles firstResponseTime = Cycles(0));

    RequestStatus makeRequest(PacketPtr pkt);
	//@hxm***********************************************************************************
	//using for slicc issuing requset after synchroniziton
	void makeLockWriteRequest() ;
 	void flushCallback();
	void PTECallback();
	void startdelaywritethrough(const Address& address);
	void DelayWTRemove(const Address& address);
	void checkL1cacheAddress(const Address& address,DataBlock& dblk,const MachineID& Machinenum);
	bool checkDMAAddress(const Address& address);
	//@hxm************************************************************************************************
    bool empty() const;
    int outstandingCount() const { return m_outstanding_count; }
    bool isDeadlockEventScheduled() const
    { return deadlockCheckEvent.scheduled(); }

    void descheduleDeadlockEvent()
    { deschedule(deadlockCheckEvent); }

    void print(std::ostream& out) const;
    void checkCoherence(const Address& address);

    void markRemoved();
    void removeRequest(SequencerRequest* request);
    void evictionCallback(const Address& address);
	
	//@hxm************************************************************************************************
	void reordShareAddress(const Address& address);
	void reordPrivateAddress(const Address& address);
	//using this function please invote the function below to sure the whether or not empty
	uint64_t getShareAddress();
	uint64_t getPrivateAddress();
	uint64_t getPTEAddress();
	inline bool ShareIsempty(){ return m_ShareAddress.empty(); }
	inline int getShareAddress_num(){ return m_ShareAddress_num; }
	inline int getPrivateAddress_num(){ return m_PrivateAddress_num; }
	inline int getPTEAddress_num(){ return m_PTEAddress_num; }
	void moveShareAddress(const Address& address);
	void movePrivateAddress(const Address& address);
	//@hxm********************************************************************************************

    void invalidateSC(const Address& address);
    void recordRequestType(SequencerRequestType requestType);
    Stats::Histogram& getOutstandReqHist() { return m_outstandReqHist; }

    Stats::Histogram& getLatencyHist() { return m_latencyHist; }
    Stats::Histogram& getTypeLatencyHist(uint32_t t)
    { return *m_typeLatencyHist[t]; }

    Stats::Histogram& getHitLatencyHist() { return m_hitLatencyHist; }
    Stats::Histogram& getHitTypeLatencyHist(uint32_t t)
    { return *m_hitTypeLatencyHist[t]; }

    Stats::Histogram& getHitMachLatencyHist(uint32_t t)
    { return *m_hitMachLatencyHist[t]; }

    Stats::Histogram& getHitTypeMachLatencyHist(uint32_t r, uint32_t t)
    { return *m_hitTypeMachLatencyHist[r][t]; }

    Stats::Histogram& getMissLatencyHist()
    { return m_missLatencyHist; }
    Stats::Histogram& getMissTypeLatencyHist(uint32_t t)
    { return *m_missTypeLatencyHist[t]; }

    Stats::Histogram& getMissMachLatencyHist(uint32_t t) const
    { return *m_missMachLatencyHist[t]; }

    Stats::Histogram&
    getMissTypeMachLatencyHist(uint32_t r, uint32_t t) const
    { return *m_missTypeMachLatencyHist[r][t]; }

    Stats::Histogram& getIssueToInitialDelayHist(uint32_t t) const
    { return *m_IssueToInitialDelayHist[t]; }

    Stats::Histogram&
    getInitialToForwardDelayHist(const MachineType t) const
    { return *m_InitialToForwardDelayHist[t]; }

    Stats::Histogram&
    getForwardRequestToFirstResponseHist(const MachineType t) const
    { return *m_ForwardToFirstResponseDelayHist[t]; }

    Stats::Histogram&
    getFirstResponseToCompletionDelayHist(const MachineType t) const
    { return *m_FirstResponseToCompletionDelayHist[t]; }

    Stats::Counter getIncompleteTimes(const MachineType t) const
    { return m_IncompleteTimes[t]; }

  private:
    void issueRequest(PacketPtr pkt, RubyRequestType type);
    void hitCallback(SequencerRequest* request, DataBlock& data,
                     bool llscSuccess,
                     const MachineType mach, const bool externalHit,
                     const Cycles initialRequestTime,
                     const Cycles forwardRequestTime,
                     const Cycles firstResponseTime);

    void recordMissLatency(const Cycles t, const RubyRequestType type,
                           const MachineType respondingMach,
                           bool isExternalHit, Cycles issuedTime,
                           Cycles initialRequestTime,
                           Cycles forwardRequestTime, Cycles firstResponseTime,
                           Cycles completionTime);

    RequestStatus insertRequest(PacketPtr pkt, RubyRequestType request_type);
    bool handleLlsc(const Address& address, SequencerRequest* request);
	

    // Private copy constructor and assignment operator
    Sequencer(const Sequencer& obj);
    Sequencer& operator=(const Sequencer& obj);

  private:
    int m_max_outstanding_requests;
    Cycles m_deadlock_threshold;
	
    CacheMemory* m_dataCache_ptr;
    CacheMemory* m_instCache_ptr;
	PageTableBuffer* m_pagetablebuffer;

    //@hxm***********************************************************
    //use for recording the RMW LOCK package
	PacketPtr m_lockWrite_ptr;
	PacketPtr flushpkt;
	PacketPtr m_triggerpte_ptr;
	RubyRequestType m_third_type;
	RubyRequestType m_primary_type;
	//use ro record the num sharedata with flush
    int m_ShareAddress_num;
	int m_PrivateAddress_num;
	int m_PTEAddress_num;
	//using for record the share address ,then flush it 
	std::set<uint64_t> m_ShareAddress;
	std::set<uint64_t> m_PrivateAddress;
	std::set<uint64_t> m_PTEAddress;
    std::set<PacketPtr> m_packet_ptr;
	//std::queue<uint64_t> m_delaywritethrough;
	//std::queue<uint64_t> m_delaycycle;
	std::map<uint64_t,uint64_t> m_delaywritethrough;
	bool triggerflush;
        uint8_t pwtnum;
	//@hxm******************************************************
    typedef m5::hash_map<Address, SequencerRequest*> RequestTable;
    RequestTable m_writeRequestTable;
    RequestTable m_readRequestTable;
    // Global outstanding request count, across all request tables
    int m_outstanding_count;
    bool m_deadlock_check_scheduled;
    //! Counters for recording aliasing information.
    Stats::Scalar m_store_waiting_on_load;
    Stats::Scalar m_store_waiting_on_store;
    Stats::Scalar m_load_waiting_on_store;
    Stats::Scalar m_load_waiting_on_load;


    bool m_usingNetworkTester;
    //! Histogram for number of outstanding requests per cycle.
    Stats::Histogram m_outstandReqHist;

    //! Histogram for holding latency profile of all requests.
    Stats::Histogram m_latencyHist;
    std::vector<Stats::Histogram *> m_typeLatencyHist;

    //! Histogram for holding latency profile of all requests that
    //! hit in the controller connected to this sequencer.
    Stats::Histogram m_hitLatencyHist;
    std::vector<Stats::Histogram *> m_hitTypeLatencyHist;

    //! Histograms for profiling the latencies for requests that
    //! did not required external messages.
    std::vector<Stats::Histogram *> m_hitMachLatencyHist;
    std::vector< std::vector<Stats::Histogram *> > m_hitTypeMachLatencyHist;

    //! Histogram for holding latency profile of all requests that
    //! miss in the controller connected to this sequencer.
    Stats::Histogram m_missLatencyHist;
    std::vector<Stats::Histogram *> m_missTypeLatencyHist;

    //! Histograms for profiling the latencies for requests that
    //! required external messages.
    std::vector<Stats::Histogram *> m_missMachLatencyHist;
    std::vector< std::vector<Stats::Histogram *> > m_missTypeMachLatencyHist;

    //! Histograms for recording the breakdown of miss latency
    std::vector<Stats::Histogram *> m_IssueToInitialDelayHist;
    std::vector<Stats::Histogram *> m_InitialToForwardDelayHist;
    std::vector<Stats::Histogram *> m_ForwardToFirstResponseDelayHist;
    std::vector<Stats::Histogram *> m_FirstResponseToCompletionDelayHist;
    std::vector<Stats::Counter> m_IncompleteTimes;

    class SequencerWakeupEvent : public Event
    {
      private:
        Sequencer *m_sequencer_ptr;

      public:
        SequencerWakeupEvent(Sequencer *_seq) : m_sequencer_ptr(_seq) {}
        void process() { m_sequencer_ptr->wakeup(); }
        const char *description() const { return "Sequencer deadlock check"; }
    };

    SequencerWakeupEvent deadlockCheckEvent;

   class SequencerDelayWTEvent : public Event
    {
      private:
        Sequencer *m_sequencer_ptr;

      public:
        SequencerDelayWTEvent(Sequencer *_seq) : m_sequencer_ptr(_seq) {}
        void process() { m_sequencer_ptr->delaywritethrough(); }
        const char *description() const { return "Sequencer delay write through"; }
    };

    SequencerDelayWTEvent delaywritethrougEvent;	
};

inline std::ostream&
operator<<(std::ostream& out, const Sequencer& obj)
{
    obj.print(out);
    out << std::flush;
    return out;
}

#endif // __MEM_RUBY_SYSTEM_SEQUENCER_HH__
