/*
 * Copyright 2014 Google, Inc.
 * Copyright (c) 2010-2013 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2002-2005 The Regents of The University of Michigan
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
 *
 * Authors: Steve Reinhardt
 */

#include "arch/locked_mem.hh"
#include "arch/mmapped_ipr.hh"
#include "arch/x86/pagetable.hh"
#include "arch/utility.hh"
#include "base/bigint.hh"
#include "config/the_isa.hh"
#include "cpu/simple/timing.hh"
#include "cpu/exetrace.hh"
#include "debug/Config.hh"
#include "debug/Drain.hh"
#include "debug/ExecFaulting.hh"
#include "debug/SimpleCPU.hh"
#include "debug/hxmDecode.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "params/TimingSimpleCPU.hh"
#include "sim/faults.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"

#include "debug/Mwait.hh"

using namespace std;
using namespace TheISA;

void
TimingSimpleCPU::init()
{
    BaseCPU::init();

    // Initialise the ThreadContext's memory proxies
    tcBase()->initMemProxies(tcBase());

    if (FullSystem && !params()->switched_out) {
        for (int i = 0; i < threadContexts.size(); ++i) {
            ThreadContext *tc = threadContexts[i];
            // initialize CPU, including PC
            TheISA::initCPU(tc, _cpuId);
        }
    }
}

void
TimingSimpleCPU::TimingCPUPort::TickEvent::schedule(PacketPtr _pkt, Tick t)
{
    pkt = _pkt;
    cpu->schedule(this, t);
}

TimingSimpleCPU::TimingSimpleCPU(TimingSimpleCPUParams *p)
    : BaseSimpleCPU(p), fetchTranslation(this), icachePort(this),
      dcachePort(this), ifetch_pkt(NULL), dcache_pkt(NULL),rcache_pkt(NULL),previousCycle(0),
      fetchEvent(this), drainManager(NULL)
{
    _status = Idle;
}



TimingSimpleCPU::~TimingSimpleCPU()
{
}

unsigned int
TimingSimpleCPU::drain(DrainManager *drain_manager)
{
    assert(!drainManager);
    if (switchedOut())
        return 0;

    if (_status == Idle ||
        (_status == BaseSimpleCPU::Running && isDrained())) {
        DPRINTF(Drain, "No need to drain.\n");
        return 0;
    } else {
        drainManager = drain_manager;
        DPRINTF(Drain, "Requesting drain: %s\n", pcState());

        // The fetch event can become descheduled if a drain didn't
        // succeed on the first attempt. We need to reschedule it if
        // the CPU is waiting for a microcode routine to complete.
        if (_status == BaseSimpleCPU::Running && !fetchEvent.scheduled())
            schedule(fetchEvent, clockEdge());

        return 1;
    }
}

void
TimingSimpleCPU::drainResume()
{
    assert(!fetchEvent.scheduled());
    assert(!drainManager);
    if (switchedOut())
        return;

    DPRINTF(SimpleCPU, "Resume\n");
    verifyMemoryMode();

    assert(!threadContexts.empty());
    if (threadContexts.size() > 1)
        fatal("The timing CPU only supports one thread.\n");

    if (thread->status() == ThreadContext::Active) {
        schedule(fetchEvent, nextCycle());
        _status = BaseSimpleCPU::Running;
        notIdleFraction = 1;
    } else {
        _status = BaseSimpleCPU::Idle;
        notIdleFraction = 0;
    }
}

bool
TimingSimpleCPU::tryCompleteDrain()
{
    if (!drainManager)
        return false;

    DPRINTF(Drain, "tryCompleteDrain: %s\n", pcState());
    if (!isDrained())
        return false;

    DPRINTF(Drain, "CPU done draining, processing drain event\n");
    drainManager->signalDrainDone();
    drainManager = NULL;

    return true;
}

void
TimingSimpleCPU::switchOut()
{
    BaseSimpleCPU::switchOut();

    assert(!fetchEvent.scheduled());
    assert(_status == BaseSimpleCPU::Running || _status == Idle);
    assert(!stayAtPC);
    assert(microPC() == 0);

    updateCycleCounts();
}


void
TimingSimpleCPU::takeOverFrom(BaseCPU *oldCPU)
{
    BaseSimpleCPU::takeOverFrom(oldCPU);

    previousCycle = curCycle();
}

void
TimingSimpleCPU::verifyMemoryMode() const
{
    if (!system->isTimingMode()) {
        fatal("The timing CPU requires the memory system to be in "
              "'timing' mode.\n");
    }
}

void
TimingSimpleCPU::activateContext(ThreadID thread_num)
{
    DPRINTF(SimpleCPU, "ActivateContext %d\n", thread_num);

    assert(thread_num == 0);
    assert(thread);

    assert(_status == Idle);

    notIdleFraction = 1;
    _status = BaseSimpleCPU::Running;

    // kick things off by initiating the fetch of the next instruction
    schedule(fetchEvent, clockEdge(Cycles(0)));
}


void
TimingSimpleCPU::suspendContext(ThreadID thread_num)
{
    DPRINTF(SimpleCPU, "SuspendContext %d\n", thread_num);

    assert(thread_num == 0);
    assert(thread);

    if (_status == Idle)
        return;

    assert(_status == BaseSimpleCPU::Running);

    // just change status to Idle... if status != Running,
    // completeInst() will not initiate fetch of next instruction.

    notIdleFraction = 0;
    _status = Idle;
}

bool
TimingSimpleCPU::handleReadPacket(PacketPtr pkt)
{
   // std::cout<<"handleReadPacket begin"<<std::endl;
    RequestPtr req = pkt->req;
    rcache_pkt = pkt;
    // We're about the issues a locked load, so tell the monitor
    // to start caring about this address
    if (pkt->isRead() && pkt->req->isLLSC()) {
		DPRINTF(SimpleCPU,"LLSC enter\n");
        TheISA::handleLockedRead(thread, pkt->req);
    }
    if (req->isMmappedIpr()) {
		DPRINTF(SimpleCPU,"MmappedIpr enter\n");
        Cycles delay = TheISA::handleIprRead(thread->getTC(), pkt);
        new IprEvent(pkt, this, clockEdge(delay));
        _status = DcacheWaitResponse;
        dcache_pkt = NULL;
    } else if(req->getTrigger()){
     //hxm***************************************************************
	   //invoke the send IPI function 	    
		TlbEntry *entry = thread->dtb->lookup(req->getVaddr());
		assert((entry->paddr & 0xfff) == 0);
		uint64_t i_addr = entry->paddr | ((uint64_t)_cpuId) | (uint64_t)(3<<8) ;//unit Dcache memory read unit 3 
		std::cout<<" handleReadPacket i_addr = "<<std::hex<<i_addr<<" req->paddr = "<<std::hex<<req->getPaddr()<<std::endl;
		uint8_t keepercpu = entry->keeper;
		interrupts->sendTrigger(keepercpu,i_addr);
		req->setTrigger(false);
		entry->trigger = false;
		_status = DcacheWaitResponse;
	//hxm***************************************************************
    }else if (!dcachePort.sendTimingReq(pkt)) {
		//@hxm*********************************************************
		//trigger by 2M page
		if(pkt->trigger){
			//invoke the send IPI function 
			 DPRINTF(SimpleCPU,"sendTimingReq(pkt) enter trigger pdp read\n");
			 Addr paddr_tri = req->getPaddr() & 0xfffffffffffff000;
			 uint64_t i_addr = paddr_tri| ((uint64_t)_cpuId) |(3<<8);//unit 1 :ifetch
			 std::cout<<"in timing.cc sendFetch i_addr =0x"<<std::hex<<i_addr<<" paddr = "<<std::hex<<req->getPaddr()<<std::endl;//send page base address
			 std::cout<<"in timing.cc sendFetch keeper=0x"<<std::hex<<(int)pkt->keeper_pkt<<std::endl;
			 interrupts->sendTrigger((uint8_t)(pkt->keeper_pkt),i_addr);
			 pkt->trigger = false;
			 _status = DcacheWaitResponse;
		}else {
			assert(false);
			// Need to wait for retry
			_status = DcacheRetry;
            dcache_pkt = pkt;
		}
    } else {
        _status = DcacheWaitResponse;
        // memory system takes ownership of packet
        dcache_pkt = NULL;
    }
   // std::cout<<"handleReadPacket end"<<std::endl;
    return dcache_pkt == NULL;
}

//@hxm********************************************************
bool
TimingSimpleCPU::triggerReadPacket()
{
   PacketPtr pkt = rcache_pkt;
   DPRINTF(SimpleCPU, "triggerReadPacket begin\n");
   pkt->triggerok = true;
   if (!dcachePort.sendTimingReq(pkt)) {
        _status = DcacheRetry;
        dcache_pkt = pkt;
    } else {
        _status = DcacheWaitResponse;
        // memory system takes ownership of packet
        dcache_pkt = NULL;
    }
   // std::cout<<"handleReadPacket end"<<std::endl;
    return dcache_pkt == NULL;
}
//@hxm********************************************************

void
TimingSimpleCPU::sendData(RequestPtr req, uint8_t *data, uint64_t *res,
                          bool read)
{
	DPRINTF(SimpleCPU, "sendData begin\n");
    PacketPtr pkt = buildPacket(req, read);
    pkt->dataDynamic<uint8_t>(data);	
    if (req->getFlags().isSet(Request::NO_ACCESS)) {
        assert(!dcache_pkt);
        pkt->makeResponse();
        completeDataAccess(pkt);
    } else if (read) {
        handleReadPacket(pkt);
    } else {
        bool do_access = true;  // flag to suppress cache access

        if (req->isLLSC()) {
            do_access = TheISA::handleLockedWrite(thread, req, dcachePort.cacheBlockMask);
        } else if (req->isCondSwap()) {
            assert(res);
            req->setExtraData(*res);
        }
        if (do_access) {
            dcache_pkt = pkt;
            handleWritePacket();
        } else {
            _status = DcacheWaitResponse;
            completeDataAccess(pkt);
        }
    }
	DPRINTF(SimpleCPU, "sendData end\n");
}

void
TimingSimpleCPU::sendSplitData(RequestPtr req1, RequestPtr req2,
                               RequestPtr req, uint8_t *data, bool read)
{
    PacketPtr pkt1, pkt2;
    buildSplitPacket(pkt1, pkt2, req1, req2, req, data, read);
	if(pkt1->req->getTrigger()) {
		std::cout<<"in timing.cc there should be place a trigger."<<std::endl;
		assert(false);
	}
	if(pkt2->req->getTrigger()) {
		std::cout<<"in timing.cc there should be place a trigger."<<std::endl;
		assert(false);
	}
    if (req->getFlags().isSet(Request::NO_ACCESS)) {
        assert(!dcache_pkt);
        pkt1->makeResponse();
        completeDataAccess(pkt1);
    } else if (read) {
        SplitFragmentSenderState * send_state =
            dynamic_cast<SplitFragmentSenderState *>(pkt1->senderState);
        if (handleReadPacket(pkt1)) {
            send_state->clearFromParent();
            send_state = dynamic_cast<SplitFragmentSenderState *>(
                    pkt2->senderState);
            if (handleReadPacket(pkt2)) {
                send_state->clearFromParent();
            }
        }
    } else {
        dcache_pkt = pkt1;
        SplitFragmentSenderState * send_state =
            dynamic_cast<SplitFragmentSenderState *>(pkt1->senderState);
        if (handleWritePacket()) {
            send_state->clearFromParent();
            dcache_pkt = pkt2;
            send_state = dynamic_cast<SplitFragmentSenderState *>(
                    pkt2->senderState);
            if (handleWritePacket()) {
                send_state->clearFromParent();
            }
        }
    }
}

void
TimingSimpleCPU::translationFault(const Fault &fault)
{
    // fault may be NoFault in cases where a fault is suppressed,
    // for instance prefetches.
    updateCycleCounts();

    if (traceData) {
        // Since there was a fault, we shouldn't trace this instruction.
        delete traceData;
        traceData = NULL;
    }

    postExecute();

    advanceInst(fault);
}

PacketPtr
TimingSimpleCPU::buildPacket(RequestPtr req, bool read)
{
    return read ? Packet::createRead(req) : Packet::createWrite(req);
}

void
TimingSimpleCPU::buildSplitPacket(PacketPtr &pkt1, PacketPtr &pkt2,
        RequestPtr req1, RequestPtr req2, RequestPtr req,
        uint8_t *data, bool read)
{
    pkt1 = pkt2 = NULL;
  
    assert(!req1->isMmappedIpr() && !req2->isMmappedIpr());

    if (req->getFlags().isSet(Request::NO_ACCESS)) {
        pkt1 = buildPacket(req, read);
        return;
    }

    pkt1 = buildPacket(req1, read);
    pkt2 = buildPacket(req2, read);

    PacketPtr pkt = new Packet(req, pkt1->cmd.responseCommand());

    pkt->dataDynamic<uint8_t>(data);
    pkt1->dataStatic<uint8_t>(data);
    pkt2->dataStatic<uint8_t>(data + req1->getSize());

    SplitMainSenderState * main_send_state = new SplitMainSenderState;
    pkt->senderState = main_send_state;
    main_send_state->fragments[0] = pkt1;
    main_send_state->fragments[1] = pkt2;
    main_send_state->outstanding = 2;
    pkt1->senderState = new SplitFragmentSenderState(pkt, 0);
    pkt2->senderState = new SplitFragmentSenderState(pkt, 1);
}

Fault
TimingSimpleCPU::readMem(Addr addr, uint8_t *data,
                         unsigned size, unsigned flags)
{
    DPRINTF(SimpleCPU, "readMem begin addr=%#x size =%d\n",addr,size);
    Fault fault;
    const int asid = 0;
    const ThreadID tid = 0;
    const Addr pc = thread->instAddr();
    unsigned block_size = cacheLineSize();
    BaseTLB::Mode mode = BaseTLB::Read;

    if (traceData)
        traceData->setMem(addr, size, flags);

    RequestPtr req  = new Request(asid, addr, size,
                                  flags, dataMasterId(), pc, _cpuId, tid);
     //@hxm=================================================
     std::string _mnemonic_;
     _mnemonic_ = curStaticInst->getName();
     if( _mnemonic_ == "ld"){
              DPRINTF(hxmDecode,"in timing.cc curStaticInst->getname():%s.OK\n",curStaticInst->getName());
              TheISA::PCState pcState = thread->pcState();
              if(!isRomMicroPC(pcState.microPC()))
              {
                      _mnemonic_ = curMacroStaticInst ->getName();
                      if(_mnemonic_ == "cmp") {
			 DPRINTF(hxmDecode,"in timing.cc curMacroStaticInst->getname():%s.OK\n",curMacroStaticInst->getName());
		         req -> setFalseldst(true);	        	
		      } else
                         DPRINTF(hxmDecode,"in timing.cc curMacroStaticInst->getname():%s.NO\n",curMacroStaticInst->getName());
              }
      }
     //@hxm========================================================================================================================
    req ->taskId(taskId());

    Addr split_addr = roundDown(addr + size - 1, block_size);
    assert(split_addr <= addr || split_addr - addr < block_size);

    _status = DTBWaitResponse;
    if (split_addr > addr) {
		DPRINTF(SimpleCPU, "enter readMem split read \n");
        RequestPtr req1, req2;
        assert(!req->isLLSC() && !req->isSwap());
        req->splitOnVaddr(split_addr, req1, req2);

        WholeTranslationState *state =
            new WholeTranslationState(req, req1, req2, new uint8_t[size],
                                      NULL, mode);
        DataTranslation<TimingSimpleCPU *> *trans1 =
            new DataTranslation<TimingSimpleCPU *>(this, state, 0);
        DataTranslation<TimingSimpleCPU *> *trans2 =
            new DataTranslation<TimingSimpleCPU *>(this, state, 1);

        thread->dtb->translateTiming(req1, tc, trans1, mode);
        thread->dtb->translateTiming(req2, tc, trans2, mode);
    } else {
        DPRINTF(SimpleCPU, "enter readMem read size:%#x\n",size);
        WholeTranslationState *state =
            new WholeTranslationState(req, new uint8_t[size], NULL, mode);
        DataTranslation<TimingSimpleCPU *> *translation
            = new DataTranslation<TimingSimpleCPU *>(this, state);
        thread->dtb->translateTiming(req, tc, translation, mode);
    }
	DPRINTF(SimpleCPU, "readMem end\n");
    return NoFault;
}

bool
TimingSimpleCPU::handleWritePacket()
{
    RequestPtr req = dcache_pkt->req;
    if (req->isMmappedIpr()) {
        Cycles delay = TheISA::handleIprWrite(thread->getTC(), dcache_pkt);
        new IprEvent(dcache_pkt, this, clockEdge(delay));
        _status = DcacheWaitResponse;
        dcache_pkt = NULL;
    }  else if( req->getTrigger()){
    //hxm***************************************************************
	   //invoke the send IPI function 
		TlbEntry *entry = thread->dtb->lookup(req->getVaddr());
		assert((entry->paddr & 0xfff) == 0);//unit store unit 1
		uint64_t i_addr = entry->paddr | ((uint64_t)_cpuId | (2<<8));
		std::cout<<" handleWritePacket i_addr = "<<std::hex<<i_addr<<" req->paddr = "<<std::hex<<req->getPaddr()<<std::endl;
		std::cout<<" handleWritePacket entry->keeper = "<<std::hex<<((int)entry->keeper)<<std::endl;
		interrupts->sendTrigger((entry->keeper),i_addr);
		_status = DcacheWaitResponse;
		req->setTrigger(false);
		entry->trigger = false;
		return true;
	//hxm***************************************************************
	}else if (!dcachePort.sendTimingReq(dcache_pkt)) {
		//@hxm*********************************************************
		//trigger by 2M page
			if(dcache_pkt->trigger){
				 DPRINTF(SimpleCPU,"sendTimingReq(pkt) enter trigger pdp write\n");
				//invoke the send IPI function 
				 Addr paddr_tri = req->getPaddr() & 0xfffffffffffff000;
				 uint64_t i_addr = paddr_tri| ((uint64_t)_cpuId) |(2<<8);//unit 1 :ifetch
				 std::cout<<"in timing.cc sendFetch i_addr =0x"<<std::hex<<i_addr<<" paddr = "<<std::hex<<paddr_tri<<std::endl;//send page base address
				 std::cout<<"in timing.cc sendFetch keeper=0x"<<std::hex<<(int)dcache_pkt->keeper_pkt<<std::endl;
				 interrupts->sendTrigger((uint8_t)(dcache_pkt->keeper_pkt),i_addr);
				 dcache_pkt->trigger = false;
				 _status = DcacheWaitResponse;
			}else {
			assert(false);
			// Need to wait for retry
			_status = DcacheRetry;
			}
    }else{
        _status = DcacheWaitResponse;
        // memory system takes ownership of packet
        dcache_pkt = NULL;
    }
   //std::cout<<"handleWritePacket end"<<std::endl;
    return dcache_pkt == NULL;
}

//@hxm****************************************************************************************************
bool
TimingSimpleCPU::triggerWritePacket()
{
	dcache_pkt->triggerok =true;
	if (!dcachePort.sendTimingReq(dcache_pkt)) {
        _status = DcacheRetry;
    }else{
        _status = DcacheWaitResponse;
        // memory system takes ownership of packet
        dcache_pkt = NULL;
    }
   //std::cout<<"handleWritePacket end"<<std::endl;
    return dcache_pkt == NULL;		
}


void 
TimingSimpleCPU::finishtransition(PacketPtr pkt)
{
	std::cout<<"in timing.cc finishtransition "<<std::endl;
	std::cout<<"in timing.cc finishtransition triggerCPU="<<(int)(interrupts->triggerCPU)<<std::endl;
	std::cout<<"in timing.cc finishtransition triggerUnit="<<(int)(interrupts->triggerUnit)<<std::endl;
	interrupts->sendTrigger((uint8_t)(interrupts->triggerCPU),(Addr)(interrupts->triggerUnit));
	interrupts->triggerCPU = 0;
	assert(interrupts->pendingTransition);
	interrupts->pendingTransition = false;
	if(pkt){
		delete pkt->req;
		delete pkt;
	}
}

void 
TimingSimpleCPU::buildtransition(void)
{
    assert(!interrupts->pendingTransition);
	if(interrupts->pendingNum > 0){
		interrupts->pendingTransition = true; 
		Addr trigAddr = interrupts->pendingAddr.front();
	    interrupts->pendingAddr.pop();
	    interrupts->pendingNum--;
		interrupts->triggerCPU = (uint8_t)(trigAddr & 0xff);//low 8 bit deserve cpu_id
		interrupts->triggerUnit = (uint8_t)((trigAddr & 0xf00)>>8);//8~11bits decide cpu_unit mybe icache0/dcache(read2,write1)	
		std::cout<<"in timing.cc buildtransition triggerAddr =0x "<<std::hex<<trigAddr<<std::endl;
		interrupts->triggerAddr = trigAddr & (mask(40)<<12);
		std::cout<<"in timing.cc buildtransition triggerAddr =0x "<<std::hex<<interrupts->triggerAddr<<std::endl;
		DPRINTF(SimpleCPU, "Got Trigger Interrupt message with triggerAddr= %#x and triggerCPU= %d.\n",interrupts->triggerAddr,interrupts->triggerCPU);
	 }
}


//@hxm****************************************************************************************************

Fault
TimingSimpleCPU::writeMem(uint8_t *data, unsigned size,
                          Addr addr, unsigned flags, uint64_t *res)
{
	DPRINTF(SimpleCPU, "writeMem begin size=%d\n",size);
    uint8_t *newData = new uint8_t[size];
    const int asid = 0;
    const ThreadID tid = 0;
    const Addr pc = thread->instAddr();
    unsigned block_size = cacheLineSize();
    BaseTLB::Mode mode = BaseTLB::Write;

    if (data == NULL) {
        assert(flags & Request::CACHE_BLOCK_ZERO);
        // This must be a cache block cleaning request
        memset(newData, 0, size);
    } else {
        memcpy(newData, data, size);
    }

    if (traceData)
        traceData->setMem(addr, size, flags);

    RequestPtr req = new Request(asid, addr, size,
                                 flags, dataMasterId(), pc, _cpuId, tid);

    req->taskId(taskId());

    Addr split_addr = roundDown(addr + size - 1, block_size);
    assert(split_addr <= addr || split_addr - addr < block_size);

    _status = DTBWaitResponse;
    if (split_addr > addr) {
        RequestPtr req1, req2;
        assert(!req->isLLSC() && !req->isSwap());
        req->splitOnVaddr(split_addr, req1, req2);

        WholeTranslationState *state =
            new WholeTranslationState(req, req1, req2, newData, res, mode);
        DataTranslation<TimingSimpleCPU *> *trans1 =
            new DataTranslation<TimingSimpleCPU *>(this, state, 0);
        DataTranslation<TimingSimpleCPU *> *trans2 =
            new DataTranslation<TimingSimpleCPU *>(this, state, 1);

        thread->dtb->translateTiming(req1, tc, trans1, mode);
        thread->dtb->translateTiming(req2, tc, trans2, mode);
    } else {
        WholeTranslationState *state =
            new WholeTranslationState(req, newData, res, mode);
        DataTranslation<TimingSimpleCPU *> *translation =
            new DataTranslation<TimingSimpleCPU *>(this, state);
        thread->dtb->translateTiming(req, tc, translation, mode);
    }
	DPRINTF(SimpleCPU, "writeMem end\n");
    // Translation faults will be returned via finishTranslation()
    return NoFault;
}


void
TimingSimpleCPU::finishTranslation(WholeTranslationState *state)
{
    _status = BaseSimpleCPU::Running;

    if (state->getFault() != NoFault) {
        if (state->isPrefetch()) {
            state->setNoFault();
        }
        delete [] state->data;
        state->deleteReqs();
        translationFault(state->getFault());
    } else {
        if (!state->isSplit) {
			DPRINTF(SimpleCPU, "in timing.cc finishTranslation now sendData\n");
			DPRINTF(SimpleCPU, "in timing.cc finishTranslation now sendData size = %#x\n",state->mainReq->getSize());
            sendData(state->mainReq, state->data, state->res,
                     state->mode == BaseTLB::Read);
        } else {
            std::cout<<"in timing.cc*********begin********* send SplitData"<<std::endl;
            sendSplitData(state->sreqLow, state->sreqHigh, state->mainReq,
                          state->data, state->mode == BaseTLB::Read);
		    std::cout<<"in timing.cc*********end*********** send SplitData"<<std::endl;
			DPRINTF(SimpleCPU, "in timing.cc finishTranslation now send SplitData\n");
			//assert(false);
        }
    }

    delete state;
}

void
TimingSimpleCPU::fetch()
{
    DPRINTF(SimpleCPU, "Fetch\n");
	if (!curStaticInst || !curStaticInst->isDelayedCommit()){
		 //@hxm*******************************************************************************
		 if(interrupts->pendingTransition){
			 std::cout<<"@hxm in timing.cc enter fetch Transition"<<std::endl;
			  //send pkt,update TLB
			  std::cout<<"@hxm in timing.cc triggerAddr=0x"<<interrupts->triggerAddr<<std::endl;
			 thread->itb->flushAll();//to do list
			 thread->dtb->flushAll();
			 //send trigger packet
			 RequestPtr req = new Request(interrupts->triggerAddr,sizeof(unsigned),Request::INVALIDSHARE, instMasterId());
			 req->taskId(taskId());
			 req->setThreadContext(_cpuId,0);		 
			 std::cout<<"@hxm in timing.cc triggerpaddr="<<interrupts->triggerAddr<<std::endl;
			 PacketPtr triggerPacket = new Packet(req,MemCmd::InvalidShareReq);
			 triggerPacket->allocate();
			 triggerPacket->set<unsigned>(0);
			 icachePort.sendTimingReq(triggerPacket);  
			 //interrupts ->pendingTransition = false;
			 std::cout<<"@hxm in timing.cc fetch trigger send packet is over"<<std::endl;
			 return;
		 }
		//@hxm*******************************************************************************
	}
    if (!curStaticInst || !curStaticInst->isDelayedCommit()) checkForInterrupts();	
        checkPcEventQueue();
    // We must have just got suspended by a PC event
    if (_status == Idle)
        return;
    TheISA::PCState pcState = thread->pcState();
    bool needToFetch = !isRomMicroPC(pcState.microPC()) && !curMacroStaticInst;

    if (needToFetch) {
        _status = BaseSimpleCPU::Running;
        Request *ifetch_req = new Request();
        ifetch_req->taskId(taskId());
        ifetch_req->setThreadContext(_cpuId, /* thread ID */ 0);
        setupFetchRequest(ifetch_req);
        DPRINTF(SimpleCPU, "Translating address %#x\n", ifetch_req->getVaddr());
        thread->itb->translateTiming(ifetch_req, tc, &fetchTranslation,
                BaseTLB::Execute);
    } else {
        _status = IcacheWaitResponse;
        completeIfetch(NULL);

        updateCycleCounts();
    }
}

void
TimingSimpleCPU::sendFetch(const Fault &fault, RequestPtr req,
                           ThreadContext *tc)
{
    if (fault == NoFault) {
        DPRINTF(SimpleCPU, "Sending fetch for addr %#x(pa: %#x)\n",
                req->getVaddr(), req->getPaddr());
        ifetch_pkt = new Packet(req, MemCmd::ReadReq);
        ifetch_pkt->dataStatic(&inst);
        DPRINTF(SimpleCPU, " -- pkt addr: %#x\n", ifetch_pkt->getAddr());
        //get trigger from the packet that is seted in pagetable_walker
        if( req->getTrigger()){//must be 4KB page
           //invoke the send IPI function 
            TlbEntry *entry = thread->itb->lookup(req->getVaddr());
		   	assert((entry->paddr & 0xfff)==0);
		    std::cout<<"in timing.cc sendFetch paddr = "<<std::hex<<req->getPaddr()<<std::endl;
		    uint64_t i_addr = entry->paddr | ((uint64_t)_cpuId) |(1<<8);//unit 1 :ifetch
		    std::cout<<"in timing.cc sendFetch i_addr =0x"<<std::hex<<i_addr<<" paddr = "<<std::hex<<entry->paddr<<std::endl;//send page base address
		    std::cout<<"in timing.cc sendFetch keeper=0x"<<std::hex<<(int)entry->keeper<<std::endl;
			interrupts->sendTrigger((uint8_t)(entry->keeper),i_addr);
			req->setTrigger(false);
			entry->trigger = false;
			_status = IcacheWaitResponse;
		}else{	
			if (!icachePort.sendTimingReq(ifetch_pkt)) {//may be 2M page so 
	    //@hxm*********************************************************
	    //trigger by 2M page
				if(ifetch_pkt->trigger){
					//invoke the send IPI function 
					 DPRINTF(SimpleCPU,"sendTimingReq(pkt) enter trigger pdp ifetch\n");
					 Addr paddr_tri = req->getPaddr() & 0xfffffffffffff000;
					 uint64_t i_addr = paddr_tri| ((uint64_t)_cpuId) |(1<<8);//unit 1 :ifetch
					 std::cout<<"in timing.cc sendFetch i_addr =0x"<<std::hex<<i_addr<<" paddr = "<<std::hex<<paddr_tri<<std::endl;//send page base address
					 std::cout<<"in timing.cc sendFetch keeper=0x"<<std::hex<<(int)ifetch_pkt->keeper_pkt<<std::endl;
					 interrupts->sendTrigger((uint8_t)(ifetch_pkt->keeper_pkt),i_addr);
					 ifetch_pkt->trigger = false;
					 _status = IcacheWaitResponse;
				}else {
				assert(false);
				// Need to wait for retry
			    _status = IcacheRetry;
				}
			} else {
				// Need to wait for cache to respond
				_status = IcacheWaitResponse;
				// ownership of packet transferred to memory system
				ifetch_pkt = NULL;
			}
		}	
    } else {
	    DPRINTF(SimpleCPU, "Translation of addr %#x faulted\n", req->getVaddr());
        delete req;
        // fetch fault: advance directly to next instruction (fault handler)
        _status = BaseSimpleCPU::Running;
        advanceInst(fault);
    }
    updateCycleCounts();
    DPRINTF(SimpleCPU, "Sending fetch for addr is ok\n");
}

void 
TimingSimpleCPU::triggerFetch(){
	ifetch_pkt->triggerok =true;
	if (!icachePort.sendTimingReq(ifetch_pkt)) {
		// Need to wait for retry
		std::cout<<"in timing.cc trigger Fetch!"<<std::endl;
		_status = IcacheRetry;
	} else {
		// Need to wait for cache to respond
		_status = IcacheWaitResponse;
		// ownership of packet transferred to memory system
		ifetch_pkt = NULL;
	}
    updateCycleCounts();
}

void
TimingSimpleCPU::advanceInst(const Fault &fault)
{
    if (_status == Faulting)
        return;

    if (fault != NoFault) {
        advancePC(fault);
        DPRINTF(SimpleCPU, "Fault occured, scheduling fetch event\n");
        reschedule(fetchEvent, clockEdge(), true);
        _status = Faulting;
        return;
    }


    if (!stayAtPC)
        advancePC(fault);

    if (tryCompleteDrain())
            return;

    if (_status == BaseSimpleCPU::Running) {
        // kick off fetch of next instruction... callback from icache
        // response will cause that instruction to be executed,
        // keeping the CPU running.
        fetch();
    }
}


void
TimingSimpleCPU::completeIfetch(PacketPtr pkt)
{
    DPRINTF(SimpleCPU, "Complete ICache Fetch for addr %#x\n", pkt ?
            pkt->getAddr() : 0);

    // received a response from the icache: execute the received
    // instruction
    assert(!pkt || !pkt->isError());
    assert(_status == IcacheWaitResponse);

    _status = BaseSimpleCPU::Running;

    updateCycleCounts();

    if (pkt)
        pkt->req->setAccessLatency();


    preExecute();
    /*if (curStaticInst && curStaticInst->isMemBarrier() && curStaticInst->isLastMicroop() )
       {
        // cout<<"@hxm find the instruct:"<<curStaticInst->getName()<<"\n"<<endl;
       }*/
   
    if (curStaticInst && curStaticInst->isMemRef()) {
        // load or store: just send to dcache
        DPRINTF(SimpleCPU, "Start access for addr %#x\n", pkt ? pkt->getAddr() : 0);
        Fault fault = curStaticInst->initiateAcc(this, traceData);
        // If we're not running now the instruction will complete in a dcache
        // response callback or the instruction faulted and has started an
        // ifetch
        if (_status == BaseSimpleCPU::Running) {
            if (fault != NoFault && traceData) {
                // If there was a fault, we shouldn't trace this instruction.
                delete traceData;
                traceData = NULL;
            }

            postExecute();
            // @todo remove me after debugging with legion done
            if (curStaticInst && (!curStaticInst->isMicroop() ||
                        curStaticInst->isFirstMicroop()))
                instCnt++;
			DPRINTF(SimpleCPU, "before advanceInst\n");
            advanceInst(fault);
        }
    } else if (curStaticInst) {
        // non-memory instruction: execute completely now
        Fault fault = curStaticInst->execute(this, traceData);
        DPRINTF(SimpleCPU, "Complete excute for addr %#x\n", pkt ? pkt->getAddr() : 0);
        // keep an instruction count
        if (fault == NoFault)
            countInst();
        else if (traceData && !DTRACE(ExecFaulting)) {
            delete traceData;
            traceData = NULL;
        }

        postExecute();
        // @todo remove me after debugging with legion done
        if (curStaticInst && (!curStaticInst->isMicroop() ||
                    curStaticInst->isFirstMicroop()))
            instCnt++;
        advanceInst(fault);
    } else {
        advanceInst(NoFault);
    }

    if (pkt) {
        delete pkt->req;
        delete pkt;
    }
}

void
TimingSimpleCPU::IcachePort::ITickEvent::process()
{
   if(pkt->isFlush()){
   		std::cout<<"in timing.cc icache port get flush packet"<<std::endl; 
		//call send Fetch
		cpu->finishtransition(pkt);
		//if there have another pendingTransition,build the environment
		cpu->buildtransition();
        cpu->fetch();	
	}else{
		cpu->completeIfetch(pkt);
	}
}

bool
TimingSimpleCPU::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(SimpleCPU, "Received fetch response %#x\n", pkt->getAddr());
    // we should only ever see one response per cycle since we only
    // issue a new request once this response is sunk
    assert(!tickEvent.scheduled());
    // delay processing of returned data until next CPU clock edge
    tickEvent.schedule(pkt, cpu->clockEdge());

    return true;
}

void
TimingSimpleCPU::IcachePort::recvReqRetry()
{
    // we shouldn't get a retry unless we have a packet that we're
    // waiting to transmit
    assert(cpu->ifetch_pkt != NULL);
    assert(cpu->_status == IcacheRetry);
    PacketPtr tmp = cpu->ifetch_pkt;
    if (sendTimingReq(tmp)) {
        cpu->_status = IcacheWaitResponse;
        cpu->ifetch_pkt = NULL;
    }
}

void
TimingSimpleCPU::completeDataAccess(PacketPtr pkt)
{
    // received a response from the dcache: complete the load or store
    // instruction
    DPRINTF(SimpleCPU, "start completeDataAccess\n");
	DPRINTF(SimpleCPU, "start completeDataAccess:%#x\n",pkt->getSize());
    assert(!pkt->isError());
    assert(_status == DcacheWaitResponse || _status == DTBWaitResponse ||
           pkt->req->getFlags().isSet(Request::NO_ACCESS));

    pkt->req->setAccessLatency();

    updateCycleCounts();

    if (pkt->senderState) {
        SplitFragmentSenderState * send_state =
            dynamic_cast<SplitFragmentSenderState *>(pkt->senderState);
        assert(send_state);
        delete pkt->req;
        delete pkt;
        PacketPtr big_pkt = send_state->bigPkt;
        delete send_state;
        
        SplitMainSenderState * main_send_state =
            dynamic_cast<SplitMainSenderState *>(big_pkt->senderState);
        assert(main_send_state);
        // Record the fact that this packet is no longer outstanding.
        assert(main_send_state->outstanding != 0);
        main_send_state->outstanding--;

        if (main_send_state->outstanding) {
            return;
        } else {
            delete main_send_state;
            big_pkt->senderState = NULL;
            pkt = big_pkt;
        }
    }

    _status = BaseSimpleCPU::Running;
    DPRINTF(SimpleCPU, "middle1 completeDataAccess\n");
    Fault fault = curStaticInst->completeAcc(pkt, this, traceData);

    // keep an instruction count
    if (fault == NoFault)
        countInst();
    else if (traceData) {
        // If there was a fault, we shouldn't trace this instruction.
        delete traceData;
        traceData = NULL; 
    }

    delete pkt->req;
    delete pkt;
    DPRINTF(SimpleCPU, "middle2 completeDataAccess\n");
    postExecute();
    DPRINTF(SimpleCPU, "middle3 completeDataAccess\n");
    advanceInst(fault);
	DPRINTF(SimpleCPU, "end completeDataAccess\n");
}

void
TimingSimpleCPU::updateCycleCounts()
{
    const Cycles delta(curCycle() - previousCycle);

    numCycles += delta;
    ppCycles->notify(delta);

    previousCycle = curCycle();
}

void
TimingSimpleCPU::DcachePort::recvTimingSnoopReq(PacketPtr pkt)
{
    // X86 ISA: Snooping an invalidation for monitor/mwait
    if(cpu->getAddrMonitor()->doMonitor(pkt)) {
        cpu->wakeup();
    }
    TheISA::handleLockedSnoop(cpu->thread, pkt, cacheBlockMask);
}

void
TimingSimpleCPU::DcachePort::recvFunctionalSnoop(PacketPtr pkt)
{
    // X86 ISA: Snooping an invalidation for monitor/mwait
    if(cpu->getAddrMonitor()->doMonitor(pkt)) {
        cpu->wakeup();
    }
}

bool
TimingSimpleCPU::DcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(SimpleCPU, "Received load/store response %#x\n", pkt->getAddr());

    // The timing CPU is not really ticked, instead it relies on the
    // memory system (fetch and load/store) to set the pace.
    if (!tickEvent.scheduled()) {
        // Delay processing of returned data until next CPU clock edge
        tickEvent.schedule(pkt, cpu->clockEdge());
        return true;
    } else {
        // In the case of a split transaction and a cache that is
        // faster than a CPU we could get two responses in the
        // same tick, delay the second one
        if (!retryRespEvent.scheduled())
            cpu->schedule(retryRespEvent, cpu->clockEdge(Cycles(1)));
        return false;
    }
}

void
TimingSimpleCPU::DcachePort::DTickEvent::process()
{
    cpu->completeDataAccess(pkt);
}

void
TimingSimpleCPU::DcachePort::recvReqRetry()
{
    // we shouldn't get a retry unless we have a packet that we're
    // waiting to transmit
    assert(cpu->dcache_pkt != NULL);
    assert(cpu->_status == DcacheRetry);
    PacketPtr tmp = cpu->dcache_pkt;
    if (tmp->senderState) {
        // This is a packet from a split access.
        SplitFragmentSenderState * send_state =
            dynamic_cast<SplitFragmentSenderState *>(tmp->senderState);
        assert(send_state);
        PacketPtr big_pkt = send_state->bigPkt;
        
        SplitMainSenderState * main_send_state =
            dynamic_cast<SplitMainSenderState *>(big_pkt->senderState);
        assert(main_send_state);

        if (sendTimingReq(tmp)) {
            // If we were able to send without retrying, record that fact
            // and try sending the other fragment.
            send_state->clearFromParent();
            int other_index = main_send_state->getPendingFragment();
            if (other_index > 0) {
                tmp = main_send_state->fragments[other_index];
                cpu->dcache_pkt = tmp;
                if ((big_pkt->isRead() && cpu->handleReadPacket(tmp)) ||
                        (big_pkt->isWrite() && cpu->handleWritePacket())) {
                    main_send_state->fragments[other_index] = NULL;
                }
            } else {
                cpu->_status = DcacheWaitResponse;
                // memory system takes ownership of packet
                cpu->dcache_pkt = NULL;
            }
        }
    } else if (sendTimingReq(tmp)) {
        cpu->_status = DcacheWaitResponse;
        // memory system takes ownership of packet
        cpu->dcache_pkt = NULL;
    }
}

TimingSimpleCPU::IprEvent::IprEvent(Packet *_pkt, TimingSimpleCPU *_cpu,
    Tick t)
    : pkt(_pkt), cpu(_cpu)
{
    cpu->schedule(this, t);
}

void
TimingSimpleCPU::IprEvent::process()
{
    cpu->completeDataAccess(pkt);
}

const char *
TimingSimpleCPU::IprEvent::description() const
{
    return "Timing Simple CPU Delay IPR event";
}


void
TimingSimpleCPU::printAddr(Addr a)
{
    dcachePort.printAddr(a);
}


////////////////////////////////////////////////////////////////////////
//
//  TimingSimpleCPU Simulation Object
//
TimingSimpleCPU *
TimingSimpleCPUParams::create()
{
    numThreads = 1;
    if (!FullSystem && workload.size() != 1)
        panic("only one workload allowed");
    return new TimingSimpleCPU(this);
}
