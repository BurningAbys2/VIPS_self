/*
 * Copyright (c) 1999-2012 Mark D. Hill and David A. Wood
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

#ifndef __MEM_RUBY_STRUCTURES_PAGETABLEBUFFER_HH__
#define __MEM_RUBY_STRUCTURES_PAGETABLEBUFFER_HH__

#include <string>
#include <vector>

#include "base/hashmap.hh"
#include "base/statistics.hh"
#include "mem/protocol/CacheRequestType.hh"
#include "mem/protocol/CacheResourceType.hh"
#include "mem/protocol/RubyRequest.hh"
#include "mem/ruby/common/DataBlock.hh"
#include "mem/ruby/slicc_interface/AbstractCacheEntry.hh"
#include "mem/ruby/slicc_interface/RubySlicc_ComponentMapping.hh"
#include "mem/ruby/structures/BankedArray.hh"
#include "mem/ruby/structures/LRUPolicy.hh"
#include "mem/ruby/structures/PseudoLRUPolicy.hh"
#include "mem/ruby/system/CacheRecorder.hh"
#include "params/PageTableBuffer.hh"
#include "sim/sim_object.hh"
//@hxm***************************************************************

class PageTableBuffer : public SimObject
{
	BitUnion64(EntryPageTable) 
		Bitfield<63> nx;//·ÇÖ´ÐÐ	
		Bitfield<62,53> interrupt10cpu; 
		Bitfield<52> trig;	
		Bitfield<51, 12> base;	
		Bitfield<11> prv;//private or shared	
		Bitfield<10, 9> avl;//only 8 core support now keeper used for containing the identity of the first and single processor cacheing PTE in table	
		Bitfield<8> g; //Global 	
		Bitfield<7> ps; //PAT support	
		Bitfield<6> d; //dirty	
		Bitfield<5> a; //accessed	
		Bitfield<4> pcd; //page cache disable	
		Bitfield<3> pwt; //write through	
		Bitfield<2> u; //user/superuser 
		Bitfield<1> w; //read/write 	
		Bitfield<0> p; //present
	EndBitUnion(EntryPageTable)

  public:
    typedef PageTableBufferParams Params;
    PageTableBuffer(const Params *p);
    ~PageTableBuffer();

    void init();
    // Print cache contents
    void print(std::ostream& out) ;
	//@hxm*************using for add pte************************************    
	void recordEntryAddrss(const Address& address, DataBlock& dblk,const MachineID& num,bool pdp);	
	bool checkEntryAddrss(const Address& address,MachineID& Machinenum);
	void checkcacheAddress(const Address& address,DataBlock& dblk,const MachineID& Machinenum);
	bool checkAddress(PacketPtr pkt,uint32_t id); //using in sequence.cc
	bool checkAddress(const Address& address); //using for DMA in L1 cache
	bool checkPageRequstAddress(PacketPtr pkt,uint32_t id); //using for page table walker requst checking
	
  private:
    // Private copy constructor and assignment operator
    PageTableBuffer(const PageTableBuffer& obj);
  private:
	//@hxm**************************using for store the entry for page***********************	
	//std::set<uint64_t> m_ShareEntryData;
	std::map<uint64_t,uint64_t>m_shareEntryPDPData;//vpp and entry(keeper and prvivate)
	std::map<uint64_t,uint64_t>m_shareEntryPTEData;//vpp and entry(keeper and prvivate)
	//std::map<uint64_t,uint8_t>m_shareflag;	
	//@hxm**************************using for store the entry for page***********************
};

std::ostream& operator<<(std::ostream& out,  PageTableBuffer& obj);

#endif // __MEM_RUBY_STRUCTURES_PAGETABLEBUFFER_HH__
