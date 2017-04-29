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

#include "base/intmath.hh"
#include "debug/RubyCache.hh"
#include "debug/RubyCacheTrace.hh"
#include "debug/RubyResourceStalls.hh"
#include "debug/RubyStats.hh"
#include "debug/hxmRubyPrivate.hh"
#include "mem/protocol/AccessPermission.hh"
#include "mem/ruby/structures/CacheMemory.hh"
#include "mem/ruby/system/System.hh"
#include "config/the_isa.hh"
using namespace std;

ostream&
operator<<(ostream& out, const CacheMemory& obj)
{
    obj.print(out);
    out << flush;
    return out;
}

CacheMemory *
RubyCacheParams::create()
{
    return new CacheMemory(this);
}

CacheMemory::CacheMemory(const Params *p)
    : SimObject(p),
    dataArray(p->dataArrayBanks, p->dataAccessLatency, p->start_index_bit),
    tagArray(p->tagArrayBanks, p->tagAccessLatency, p->start_index_bit)
{
    m_cache_size = p->size;
    m_latency = p->latency;
    m_cache_assoc = p->assoc;
    m_policy = p->replacement_policy;
    m_start_index_bit = p->start_index_bit;
    m_is_instruction_only_cache = p->is_icache;
    m_resource_stalls = p->resourceStalls;
}

void
CacheMemory::init()
{
    m_cache_num_sets = (m_cache_size / m_cache_assoc) /
        RubySystem::getBlockSizeBytes();
    assert(m_cache_num_sets > 1);
    m_cache_num_set_bits = floorLog2(m_cache_num_sets);
    assert(m_cache_num_set_bits > 0);

    if (m_policy == "PSEUDO_LRU")
        m_replacementPolicy_ptr =
            new PseudoLRUPolicy(m_cache_num_sets, m_cache_assoc);
    else if (m_policy == "LRU")
        m_replacementPolicy_ptr =
            new LRUPolicy(m_cache_num_sets, m_cache_assoc);
    else
        assert(false);

    m_cache.resize(m_cache_num_sets);
    for (int i = 0; i < m_cache_num_sets; i++) {
        m_cache[i].resize(m_cache_assoc);
        for (int j = 0; j < m_cache_assoc; j++) {
            m_cache[i][j] = NULL;
        }
    }
}

CacheMemory::~CacheMemory()
{
    if (m_replacementPolicy_ptr != NULL)
        delete m_replacementPolicy_ptr;
    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            delete m_cache[i][j];
        }
    }
	m_shareEntryPDPData.clear();
	m_shareEntryPTEData.clear();
}

// convert a Address to its location in the cache
int64
CacheMemory::addressToCacheSet(const Address& address) const
{
    assert(address == line_address(address));
    return address.bitSelect(m_start_index_bit,
                             m_start_index_bit + m_cache_num_set_bits - 1);
}

// Given a cache index: returns the index of the tag in a set.
// returns -1 if the tag is not found.
int
CacheMemory::findTagInSet(int64 cacheSet, const Address& tag) const
{
    assert(tag == line_address(tag));
    // search the set for the tags
    m5::hash_map<Address, int>::const_iterator it = m_tag_index.find(tag);
    if (it != m_tag_index.end())
        if (m_cache[cacheSet][it->second]->m_Permission !=
            AccessPermission_NotPresent)
            return it->second;
    return -1; // Not found
}

// Given a cache index: returns the index of the tag in a set.
// returns -1 if the tag is not found.
int
CacheMemory::findTagInSetIgnorePermissions(int64 cacheSet,
                                           const Address& tag) const
{
    assert(tag == line_address(tag));
    // search the set for the tags
    m5::hash_map<Address, int>::const_iterator it = m_tag_index.find(tag);
    if (it != m_tag_index.end())
        return it->second;
    return -1; // Not found
}

bool
CacheMemory::tryCacheAccess(const Address& address, RubyRequestType type,
                            DataBlock*& data_ptr)
{
    assert(address == line_address(address));
    DPRINTF(RubyCache, "address: %s\n", address);
    int64 cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc != -1) {
        // Do we even have a tag match?
        AbstractCacheEntry* entry = m_cache[cacheSet][loc];
        m_replacementPolicy_ptr->touch(cacheSet, loc, curTick());
        data_ptr = &(entry->getDataBlk());

        if (entry->m_Permission == AccessPermission_Read_Write) {
            return true;
        }
        if ((entry->m_Permission == AccessPermission_Read_Only) &&
            (type == RubyRequestType_LD || type == RubyRequestType_IFETCH)) {
            return true;
        }
        // The line must not be accessible
    }
    data_ptr = NULL;
    return false;
}

bool
CacheMemory::testCacheAccess(const Address& address, RubyRequestType type,
                             DataBlock*& data_ptr)
{
    assert(address == line_address(address));
    DPRINTF(RubyCache, "address: %s\n", address);
    int64 cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);

    if (loc != -1) {
        // Do we even have a tag match?
        AbstractCacheEntry* entry = m_cache[cacheSet][loc];
        m_replacementPolicy_ptr->touch(cacheSet, loc, curTick());
        data_ptr = &(entry->getDataBlk());

        return m_cache[cacheSet][loc]->m_Permission !=
            AccessPermission_NotPresent;
    }

    data_ptr = NULL;
    return false;
}

// tests to see if an address is present in the cache
bool
CacheMemory::isTagPresent(const Address& address) const
{
    assert(address == line_address(address));
    int64 cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);

    if (loc == -1) {
        // We didn't find the tag
        DPRINTF(RubyCache, "No tag match for address: %s\n", address);
        return false;
    }
    DPRINTF(RubyCache, "address: %s found\n", address);
    return true;
}

// Returns true if there is:
//   a) a tag match on this address or there is
//   b) an unused line in the same cache "way"
bool
CacheMemory::cacheAvail(const Address& address) const
{
    assert(address == line_address(address));

    int64 cacheSet = addressToCacheSet(address);

    for (int i = 0; i < m_cache_assoc; i++) {
        AbstractCacheEntry* entry = m_cache[cacheSet][i];
        if (entry != NULL) {
            if (entry->m_Address == address ||
                entry->m_Permission == AccessPermission_NotPresent) {
                // Already in the cache or we found an empty entry
                return true;
            }
        } else {
            return true;
        }
    }
    return false;
}

AbstractCacheEntry*
CacheMemory::allocate(const Address& address, AbstractCacheEntry* entry)
{
    assert(address == line_address(address));
    assert(!isTagPresent(address));
    assert(cacheAvail(address));
    DPRINTF(RubyCache, "address: %s\n", address);

    // Find the first open slot
    int64 cacheSet = addressToCacheSet(address);
    std::vector<AbstractCacheEntry*> &set = m_cache[cacheSet];
    for (int i = 0; i < m_cache_assoc; i++) {
        if (!set[i] || set[i]->m_Permission == AccessPermission_NotPresent) {
            set[i] = entry;  // Init entry
            set[i]->m_Address = address;
            set[i]->m_Permission = AccessPermission_Invalid;
            DPRINTF(RubyCache, "Allocate clearing lock for addr: %x\n",
                    address);
            set[i]->m_locked = -1;
            m_tag_index[address] = i;

            m_replacementPolicy_ptr->touch(cacheSet, i, curTick());

            return entry;
        }
    }
    panic("Allocate didn't find an available entry");
}

void
CacheMemory::deallocate(const Address& address)
{
    assert(address == line_address(address));
    assert(isTagPresent(address));
    DPRINTF(RubyCache, "address: %s\n", address);
    int64 cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc != -1) {
        delete m_cache[cacheSet][loc];
        m_cache[cacheSet][loc] = NULL;
        m_tag_index.erase(address);
    }
}

// Returns with the physical address of the conflicting cache line
Address
CacheMemory::cacheProbe(const Address& address) const
{
    assert(address == line_address(address));
    assert(!cacheAvail(address));

    int64 cacheSet = addressToCacheSet(address);
    return m_cache[cacheSet][m_replacementPolicy_ptr->getVictim(cacheSet)]->
        m_Address;
}

// looks an address up in the cache
AbstractCacheEntry*
CacheMemory::lookup(const Address& address)
{
    assert(address == line_address(address));
    int64 cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if(loc == -1) return NULL;
    return m_cache[cacheSet][loc];
}

// looks an address up in the cache
const AbstractCacheEntry*
CacheMemory::lookup(const Address& address) const
{
    assert(address == line_address(address));
    int64 cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if(loc == -1) return NULL;
    return m_cache[cacheSet][loc];
}

// Sets the most recently used bit for a cache block
void
CacheMemory::setMRU(const Address& address)
{
    int64 cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);

    if(loc != -1)
        m_replacementPolicy_ptr->touch(cacheSet, loc, curTick());
}

void
CacheMemory::recordCacheContents(int cntrl, CacheRecorder* tr) const
{
    uint64 warmedUpBlocks = 0;
    uint64 totalBlocks M5_VAR_USED = (uint64)m_cache_num_sets
                                                  * (uint64)m_cache_assoc;

    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            if (m_cache[i][j] != NULL) {
                AccessPermission perm = m_cache[i][j]->m_Permission;
                RubyRequestType request_type = RubyRequestType_NULL;
                if (perm == AccessPermission_Read_Only) {
                    if (m_is_instruction_only_cache) {
                        request_type = RubyRequestType_IFETCH;
                    } else {
                        request_type = RubyRequestType_LD;
                    }
                } else if (perm == AccessPermission_Read_Write) {
                    request_type = RubyRequestType_ST;
                }

                if (request_type != RubyRequestType_NULL) {
                    tr->addRecord(cntrl, m_cache[i][j]->m_Address.getAddress(),
                                  0, request_type,
                                  m_replacementPolicy_ptr->getLastAccess(i, j),
                                  m_cache[i][j]->getDataBlk());
                    warmedUpBlocks++;
                }
            }
        }
    }

    DPRINTF(RubyCacheTrace, "%s: %lli blocks of %lli total blocks"
            "recorded %.2f%% \n", name().c_str(), warmedUpBlocks,
            (uint64)m_cache_num_sets * (uint64)m_cache_assoc,
            (float(warmedUpBlocks)/float(totalBlocks))*100.0);
}

void
CacheMemory::print(ostream& out) const
{
    out << "Cache dump: " << name() << endl;
    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            if (m_cache[i][j] != NULL) {
                out << "  Index: " << i
                    << " way: " << j
                    << " entry: " << *m_cache[i][j] << endl;
            } else {
                out << "  Index: " << i
                    << " way: " << j
                    << " entry: NULL" << endl;
            }
        }
    }
}

void
CacheMemory::printData(ostream& out) const
{
    out << "printData() not supported" << endl;
}

void
CacheMemory::setLocked(const Address& address, int context)
{
    DPRINTF(RubyCache, "Setting Lock for addr: %x to %d\n", address, context);
    assert(address == line_address(address));
    int64 cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    assert(loc != -1);
    m_cache[cacheSet][loc]->m_locked = context;
}

void
CacheMemory::clearLocked(const Address& address)
{
    DPRINTF(RubyCache, "Clear Lock for addr: %x\n", address);
    assert(address == line_address(address));
    int64 cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    assert(loc != -1);
    m_cache[cacheSet][loc]->m_locked = -1;
}

bool
CacheMemory::isLocked(const Address& address, int context)
{
    assert(address == line_address(address));
    int64 cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    assert(loc != -1);
    DPRINTF(RubyCache, "Testing Lock for addr: %llx cur %d con %d\n",
            address, m_cache[cacheSet][loc]->m_locked, context);
    return m_cache[cacheSet][loc]->m_locked == context;
}

void
CacheMemory::regStats()
{
    m_demand_hits
        .name(name() + ".demand_hits")
        .desc("Number of cache demand hits")
        ;

    m_demand_misses
        .name(name() + ".demand_misses")
        .desc("Number of cache demand misses")
        ;

    m_demand_accesses
        .name(name() + ".demand_accesses")
        .desc("Number of cache demand accesses")
        ;

    m_demand_write_miss
        .name(name() + ".demand_write_miss")
        .desc("Number of cache demand write miss")
        ;

    m_demand_read_miss
        .name(name() + ".demand_read_misses")
        .desc("Number of cache demand read misses")
        ;

    m_demand_synchronization
        .name(name() + ".demand_synchronization")
        .desc("Number of cache demand synchronization")
        ;

    m_demand_accesses = m_demand_hits + m_demand_misses;

    m_sw_prefetches
        .name(name() + ".total_sw_prefetches")
        .desc("Number of software prefetches")
        .flags(Stats::nozero)
        ;

    m_hw_prefetches
        .name(name() + ".total_hw_prefetches")
        .desc("Number of hardware prefetches")
        .flags(Stats::nozero)
        ;

    m_prefetches
        .name(name() + ".total_prefetches")
        .desc("Number of prefetches")
        .flags(Stats::nozero)
        ;

    m_prefetches = m_sw_prefetches + m_hw_prefetches;

    m_accessModeType
        .init(RubyRequestType_NUM)
        .name(name() + ".access_mode")
        .flags(Stats::pdf | Stats::total)
        ;
    for (int i = 0; i < RubyAccessMode_NUM; i++) {
        m_accessModeType
            .subname(i, RubyAccessMode_to_string(RubyAccessMode(i)))
            .flags(Stats::nozero)
            ;
    }

    numDataArrayReads
        .name(name() + ".num_data_array_reads")
        .desc("number of data array reads")
        .flags(Stats::nozero)
        ;

    numDataArrayWrites
        .name(name() + ".num_data_array_writes")
        .desc("number of data array writes")
        .flags(Stats::nozero)
        ;

    numTagArrayReads
        .name(name() + ".num_tag_array_reads")
        .desc("number of tag array reads")
        .flags(Stats::nozero)
        ;

    numTagArrayWrites
        .name(name() + ".num_tag_array_writes")
        .desc("number of tag array writes")
        .flags(Stats::nozero)
        ;

    numTagArrayStalls
        .name(name() + ".num_tag_array_stalls")
        .desc("number of stalls caused by tag array")
        .flags(Stats::nozero)
        ;

    numDataArrayStalls
        .name(name() + ".num_data_array_stalls")
        .desc("number of stalls caused by data array")
        .flags(Stats::nozero)
        ;
}

void
CacheMemory::recordRequestType(CacheRequestType requestType)
{
    DPRINTF(RubyStats, "Recorded statistic: %s\n",
            CacheRequestType_to_string(requestType));
    switch(requestType) {
    case CacheRequestType_DataArrayRead:
        numDataArrayReads++;
        return;
    case CacheRequestType_DataArrayWrite:
        numDataArrayWrites++;
        return;
    case CacheRequestType_TagArrayRead:
        numTagArrayReads++;
        return;
    case CacheRequestType_TagArrayWrite:
        numTagArrayWrites++;
        return;
    default:
        warn("CacheMemory access_type not found: %s",
             CacheRequestType_to_string(requestType));
    }
}
//@hxm*****************************************************************
/*void 
CacheMemory::recordEntryAddrss(const Address& address,DataBlock& dblk,const MachineID& Machinenum,bool pdp)
{
    NodeID num = Machinenum.getNum();
    DPRINTF(hxmRubyPrivate, "Recorded EntryAddrss: %#016x.NodeID is %#016x\n",address.getAddress(),num);
	DPRINTF(hxmRubyPrivate, "Recorded EntryAddrss_pysical: %#016x.NodeID is %#016x\n",address.getppnAddress(),num);
	Address ppnaddress(address.getppnAddress());
	//dblk.print(std::cout);
	uint8_t data[8] = {0};
	Addr ppnfirst,ppnsecond ;//record the address and flag of keeper,private
	uint8_t shareflag=0;
	uint8_t privateflag=0;
	std::map<uint64_t,uint64_t> map_temprivate;
	std::map<uint64_t,uint64_t>::iterator iter;
	memcpy(data,dblk.getData(ppnaddress.getOffset(),8),8);
	PageTableEntryhxm pte64 = *((uint64_t*)data);
   	DPRINTF(hxmRubyPrivate, "Recorded PageTableEntryhxm: %#016x.data before.\n",(uint64_t)pte64);
	bool flagpdp = pdp && pte64.p && pte64.ps;
	bool flagpte = (!pdp) && pte64.p;
    if(flagpdp){//2M page
  		ppnfirst = (uint64_t)pte64 & (mask(31) << 21);//get physical address number
  		iter = m_shareEntryPDPData.find(ppnfirst);
		if(iter !=m_shareEntryPDPData.end()){//have data
			DPRINTF(hxmRubyPrivate, "PDP000000000000000000000000000000000000000iter->second=%#016x.\n",iter->second);
			if(!(iter ->second & 0x100)){//share
				pte64.prv = 0;
				pte64.trig = 0;
				pte64.avl = 0x3 & (iter->second);
				DPRINTF(hxmRubyPrivate, "PDP11111111111111111111111111111111111share.\n");
			}
			else if((NodeID)(iter->second & 0xff) != num)//private,(9th bit  1:private) have defferent keeper
			{
				pte64.prv = 0;//set share
				pte64.avl = 0x3 & (iter->second);//set keeper
				pte64.trig = 1;
				iter->second = (iter->second)&0xfffffffffffffeff;//set share 
				DPRINTF(hxmRubyPrivate, "PDP222222222222222222222222222222222222222setshare.\n");
			}
			else//private
			{
				pte64.prv = 1;
				pte64.avl = 0x3 & (iter->second &0xff);
				pte64.trig = 0;
				DPRINTF(hxmRubyPrivate, "PDP333333333333333333333333333333333333333keepprivate.\n");
			}
		}else{//no 2M page but may be there be some 4KB page in 2MB page,so access pte table.
		   // if(m_shareEntryPTEData.size()!= 0){//not empty
		   // find share and private ,if there are some prviate,the data will add to a special set
				for(map<uint64_t,uint64_t>::iterator i =m_shareEntryPTEData.begin();i != m_shareEntryPTEData.end();i++){
					if(((i->first)&(mask(31)<<21))==ppnfirst) {
						if(!(i->second & 0x100)){ //share,*******************************
							shareflag = 1;
							m_shareEntryPTEData.erase(i);
						}else{ //private
							privateflag = 1;
							map_temprivate.insert(map<uint64_t,uint64_t>::value_type(i->first,i->second));
							m_shareEntryPTEData.erase(i);	
						}
					}
				}
				//shareflag=1,privateflag=1.there are 4KB  private and share page in the 2M page
				if(shareflag && privateflag){
					//not only share but also private;
					// so set share,trigger,and record interrupt CPU
					// clear map_temprivate
					pte64.prv = 0;
					pte64.trig = 1;
					for(map<uint64_t,uint64_t>::iterator i =map_temprivate.begin();i != map_temprivate.end();i++){
						if((NodeID)(iter->second & 0xff) != num){
							//share,trigger,record data
							pte64.avl = 0x3 & (iter->second);	
							pte64.interrupt10cpu = pte64.interrupt10cpu |(0x1<<(0x3 & (iter->second)));
							DPRINTF(hxmRubyPrivate, "PDP In cachememory.cc PTE iter->second interruptCPU %#016x\n",iter->second);
						}
					}
					map_temprivate.clear();
				 //insert 2M page map
				   ppnsecond = ppnfirst & 0xfffffffffffffeff;//set share
				 //set keeper core current CPU,so mayby some TLB will keep deffently but no deffence
				   ppnsecond = ppnsecond|((uint64_t)num&0xff); 
				   DPRINTF(hxmRubyPrivate, "PDP In cachememory.cc Keeper: %#016x.\n",(uint64)(0xff&num));
				   m_shareEntryPDPData.insert(map<uint64_t,uint64_t>::value_type(ppnfirst,ppnsecond));
				   DPRINTF(hxmRubyPrivate, "PDP In cachememory.cc ppnfirst: %#016x.ppnsecond: %#016x\n",(uint64)(ppnfirst),(uint64)(ppnsecond));
				}else if(shareflag){
				 //only shareflag = 1;there are only 4KB share page 
				 //no 2MB page but exist 4KB share page so erase 4KB (it has done before) 
				 //set share 2M page share,no trigger,set keeper current cpu(L1cahce number)
				   pte64.prv = 0;
				   pte64.trig = 0;
				   pte64.avl = (uint64_t)num&0xff; 
				 //insert 2M page map
				   ppnsecond = ppnfirst & 0xfffffffffffffeff;//set share
				 //set keeper core current CPU,so mayby some TLB will keep deffently but no deffence
				   ppnsecond = ppnsecond|((uint64_t)num&0xff); 
				   DPRINTF(hxmRubyPrivate, "PDP In cachememory.cc Keeper: %#016x.\n",(uint64)(0xff&num));
				   m_shareEntryPDPData.insert(map<uint64_t,uint64_t>::value_type(ppnfirst,ppnsecond));
				   DPRINTF(hxmRubyPrivate, "PDP In cachememory.cc ppnfirst: %#016x.ppnsecond: %#016x\n",(uint64)(ppnfirst),(uint64)(ppnsecond));
				}else if(privateflag){
				//only privateflag = 1;there are only 4KB private page 
				//so should check the keeper,if it is different,page should set share,otherwise set private.
				//first to check keeper. 
				    pte64.prv =1;
				    pte64.trig = 0;
					for(map<uint64_t,uint64_t>::iterator i =map_temprivate.begin();i != map_temprivate.end();i++){
						if((NodeID)(iter->second & 0xff) != num){
							//share,trigger,record data
							pte64.prv = 0;
							pte64.trig = 1;
							pte64.avl = 0x3 & (iter->second);	
							pte64.interrupt10cpu = pte64.interrupt10cpu |(0x1<<(0x3 & (iter->second)));
							DPRINTF(hxmRubyPrivate, "PDP many private PTE In cachememory.cc PTE iter->second interruptCPU %#016x\n",iter->second);
						}
					}
				   map_temprivate.clear();
				   //set keeper core current CPU
				   if(pte64.prv) ppnsecond = ppnfirst|0x100;//set privateppnsecond = ppnfirst|0x100;//private,keep
				   else ppnsecond = ppnfirst & 0xfffffffffffffeff;
				   ppnsecond = ppnsecond|((uint64_t)num&0xff); 
				   DPRINTF(hxmRubyPrivate, "PDP In cachememory.cc Keeper: %#016x.\n",(uint64)(0xff&num));
				   m_shareEntryPDPData.insert(map<uint64_t,uint64_t>::value_type(ppnfirst,ppnsecond));
				   DPRINTF(hxmRubyPrivate, "PDP In cachememory.cc ppnfirst: %#016x.ppnsecond: %#016x\n",(uint64)(ppnfirst),(uint64)(ppnsecond));
				}else {
				 //shareflag = 0,privateflag = 0,so it is a nem 2M page
				 //no 2MB page ,no 4KB page 
				 //set 2M page private,set keeper current cpu(L1cahce number),no trigger,put into 2M map
				   pte64.prv = 1;
				   pte64.trig = 0;
				   pte64.avl = (uint64_t)num&0xff;
				 //insert 2M page map
				   ppnsecond = ppnfirst |0x100;//set private
				   ppnsecond = ppnsecond|((uint64_t)num&0xff);
				   assert(num <= 0xff);
				   DPRINTF(hxmRubyPrivate, "PDP In cachememory.cc Keeper: %#016x.\n",(uint64)(0xff&num));
				   m_shareEntryPDPData.insert(map<uint64_t,uint64_t>::value_type(ppnfirst,ppnsecond));
				   DPRINTF(hxmRubyPrivate, "PDP In cachememory.cc ppnfirst: %#016x.ppnsecond: %#016x\n",(uint64)(ppnfirst),(uint64)(ppnsecond));
				}

		}
		//write dblk
		*(uint64_t*)data = pte64;
		dblk.setData(data,ppnaddress.getOffset(),8);
		DPRINTF(hxmRubyPrivate, "PDP In cachememory.cc pte64: %#016x.After\n",(uint64_t)pte64);		
	}else if(flagpte){
		ppnfirst = (uint64_t)pte64 & (mask(31) << 21);//get PTE 31bit to compare with PDP map
  		iter = m_shareEntryPDPData.find(ppnfirst);
		if(iter != m_shareEntryPDPData.end()){//have data
			DPRINTF(hxmRubyPrivate, "PTE000000000000000000000000000000000000000PDPiter->second=%#016x.\n",iter->second);
			if(!(iter ->second & 0x100)){//share
				pte64.prv = 0;
				pte64.trig = 0;
				pte64.avl = 0x3 & (iter->second);
				DPRINTF(hxmRubyPrivate, "PTE11111111111111111111111111111111111PDPshare.\n");
			}
			else if((NodeID)(iter->second & 0xff) != num)//private,(9th bit  1:private) have different keeper
			{
				pte64.prv = 0;//set share
				pte64.avl = 0x3 & (iter->second);//set keeper
				pte64.trig = 1;
				iter->second = (iter->second)&0xfffffffffffffeff;//set share 
				DPRINTF(hxmRubyPrivate, "PTE222222222222222222222222222222222222222PDPsetshare.\n");
			}
			else//private
			{
				pte64.prv = 1;
				pte64.avl = 0x3 & (iter->second &0xff);
				pte64.trig = 0;
				DPRINTF(hxmRubyPrivate, "PTE333333333333333333333333333333333333333PDPkeepprivate.\n");
			}
			
		}else{//no 2M page include this 4KB page so there will do a compare with 4KB page
		  ppnfirst = (uint64_t)pte64 & (mask(40) << 12);//get PTE 31bit to compare with PDP map	
		  iter = m_shareEntryPTEData.find(ppnfirst);
		  if(iter !=m_shareEntryPTEData.end()){//have same PTE  
			  DPRINTF(hxmRubyPrivate, "PTE000000000000000000000000000000000000000iter->second=%#016x.\n",iter->second);
			  if(!(iter ->second & 0x100)){//share
				  pte64.prv = 0;
				  pte64.trig = 0;
				  pte64.avl = 0x3 & (iter->second);
				  DPRINTF(hxmRubyPrivate, "PTE11111111111111111111111111111111111share.\n");
			  }
			  else if((NodeID)(iter->second & 0xff) != num)//private,(9th bit  1:private) have different keeper
			  {
				  pte64.prv = 0;//set share
				  pte64.avl = 0x3 & (iter->second);//set keeper
				  pte64.trig = 1;
				  iter->second = (iter->second)&0xfffffffffffffeff;//set share 
				  DPRINTF(hxmRubyPrivate, "PTE222222222222222222222222222222222222222setshare.\n");
			  }
			  else//private
			  {
				  pte64.prv = 1;
				  pte64.avl = 0x3 & (iter->second &0xff);
				  pte64.trig = 0;
				  DPRINTF(hxmRubyPrivate, "PTE333333333333333333333333333333333333333keepprivate.\n");
			  }
		  }else{//no data so it is the first acsess 4KB page
			  pte64.prv = 1;;
			  pte64.avl = 0x3 & ((uint64_t)num&0xff);
			  pte64.trig = 0; //don't trigger
			  DPRINTF(hxmRubyPrivate, "PTE4444444444444444444444444444444444444444444addprivate.\n");
			  ppnsecond = ppnfirst|0x100;//private,keep
			  ppnsecond = ppnsecond|((uint64_t)num&0xff);//private,keep
			  assert(num<0xff);
			  DPRINTF(hxmRubyPrivate, "PTE In cachememory.cc Keeper: %#016x.\n",(uint64)(0xff&num));
			  m_shareEntryPTEData.insert(map<uint64_t,uint64_t>::value_type(ppnfirst,ppnsecond));
			  DPRINTF(hxmRubyPrivate, "PTE In cachememory.cc ppnfirst: %#016x.ppnsecond: %#016x\n",(uint64)(ppnfirst),(uint64)(ppnsecond));
		  }
		}
		//write dblk
		*(uint64_t*)data = pte64;
		dblk.setData(data,ppnaddress.getOffset(),8);
		DPRINTF(hxmRubyPrivate, "In cachememory.cc pte64: %#016x.After\n",(uint64_t)pte64);
	}
	DPRINTF(hxmRubyPrivate, "In cachememory.cc page fault pte64: %#016x.After\n",(uint64_t)pte64);
}*/

//@hxm*****************************************************************
/*bool 
CacheMemory::checkEntryAddrss(const Address& address,MachineID& Machinenum)
{
     uint64_t ppnfirst = address.getAddress();
	 ppnfirst = (uint64_t)ppnfirst & (mask(31) << 21);//get physical address£¬maybe 2M and 4K ->as 2M
	 std::map<uint64_t,uint64_t>::iterator iter;
     iter = m_shareEntryPDPData.find(ppnfirst);
	 if(iter !=m_shareEntryPDPData.end()){
	   if(!(iter ->second & 0x100)) return false;//share
	   else{
		Machinenum.num = (iter->second)&0xff;
		Machinenum.type = MachineType_L1Cache;
		std::cout<<"in cacheMemory.cc find EntryAddrss."<<std::endl;
	   	return true;
	   }
	  }
	 ppnfirst = (uint64_t)ppnfirst & (mask(40) << 12);
	 iter = m_shareEntryPTEData.find(ppnfirst);
	 if(iter !=m_shareEntryPTEData.end()) {
        if(!(iter ->second & 0x100)) return false;//share
        else{
			Machinenum.num = (iter->second)&0xff;
			Machinenum.type = MachineType_L1Cache;
			std::cout<<"in cacheMemory.cc find EntryAddrss."<<std::endl;
			return true;//private
		}
	 }
	 std::cout<<"In cacheMemory.cc no pagetable address="<<address.getAddress()<<",so maybe pte,pde address,maybe something wrong."<<std::endl;
	 //assert(false);
	 return false;
}*/
//@hxm*****************************************************************

bool
CacheMemory::checkResourceAvailable(CacheResourceType res, Address addr)
{
    if (!m_resource_stalls) {
        return true;
    }

    if (res == CacheResourceType_TagArray) {
        if (tagArray.tryAccess(addressToCacheSet(addr))) return true;
        else {
            DPRINTF(RubyResourceStalls,
                    "Tag array stall on addr %s in set %d\n",
                    addr, addressToCacheSet(addr));
            numTagArrayStalls++;
            return false;
        }
    } else if (res == CacheResourceType_DataArray) {
        if (dataArray.tryAccess(addressToCacheSet(addr))) return true;
        else {
            DPRINTF(RubyResourceStalls,
                    "Data array stall on addr %s in set %d\n",
                    addr, addressToCacheSet(addr));
            numDataArrayStalls++;
            return false;
        }
    } else {
        assert(false);
        return true;
    }
}
