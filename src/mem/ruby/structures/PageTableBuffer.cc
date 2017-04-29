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
#include "debug/hxmRubyPrivate.hh"
#include "mem/protocol/AccessPermission.hh"
#include "mem/ruby/structures/PageTableBuffer.hh"
#include "mem/ruby/system/System.hh"
#include "config/the_isa.hh"
using namespace std;

ostream&
operator<<(ostream& out,  PageTableBuffer& obj)
{
    obj.print(out);
    out << flush;
    return out;
}

PageTableBuffer *
PageTableBufferParams::create()
{
    return new PageTableBuffer(this);
}

PageTableBuffer::PageTableBuffer(const Params *p)
    : SimObject(p)
{
	m_shareEntryPDPData.clear();
	m_shareEntryPTEData.clear();
}

void
PageTableBuffer::init()
{
	m_shareEntryPDPData.clear();
	m_shareEntryPTEData.clear();
}

PageTableBuffer::~PageTableBuffer()
{
	m_shareEntryPDPData.clear();
	m_shareEntryPTEData.clear();
}

void
PageTableBuffer::print(ostream& out)
{
    out << "PageTableWaler: " << name() << endl;
    for (map<uint64_t,uint64_t>::iterator i =m_shareEntryPTEData.begin();i != m_shareEntryPTEData.end();i++)
		cout<<"iter->first"<<i->first<<" iter->second"<<i->second<<endl;	
}

//@hxm*****************************************************************
//using for 2M page to check by 4KB
bool
PageTableBuffer::checkAddress(PacketPtr pkt,uint32_t id)
{
     //second: |_||_||_|***|_|   |_|    |_|
     //         63 62 61   10     9      8
     //                    pdp   lock  private                     
	 uint64_t ppnsecond= 0;
     uint64_t address = pkt->getAddr();
	 std::map<uint64_t,uint64_t>::iterator iter;
	 uint64_t ppnfirst = address & (mask(40) << 12);//4KB
	 iter = m_shareEntryPTEData.find(ppnfirst);
	 if(iter !=m_shareEntryPTEData.end()) {//the 4kb page that address belongs to is accessed before
	 //check its share or private 
	        if(!(iter ->second & 0x100)) return false;//share
		    else if((NodeID)(iter->second & 0xff) != id)//private,(9th bit  1:private) have different keeper,trigger
		    {
		      if(!(pkt->triggerok)){
				pkt->keeper_pkt = 0xff & (iter->second);//set keeper
			    pkt->trigger = true;
				iter->second = (iter->second)|0x200;//set lock(private)
				DPRINTF(hxmRubyPrivate, "checkAddressPTE222222222222222222222222222222222222222setshare.\n");
			    return true;
			  }else{
				assert(iter->second & 0x200);//lock state 
				iter->second =iter->second & 0xfffffffffffffcff;//set unlock,share
				pkt->trigger = false;
				return false;//share
			  }
			}else{//private and same keeper,set private
			    assert(pkt->triggerok == false);
				pkt->trigger = false;
				return true;
			}
		}else {//no 4kb page so add it into 
	    ppnsecond = ppnfirst | 0x500;//private,unlock
	    ppnsecond = ppnsecond|((uint64_t)id & 0xff);//private,keep
	    assert(id<0xff);
	    DPRINTF(hxmRubyPrivate, "PTE In PageTableBuffer.cc Keeper: %#016x.\n",(uint64)(0xff&id));
	    m_shareEntryPTEData.insert(map<uint64_t,uint64_t>::value_type(ppnfirst,ppnsecond));
	    DPRINTF(hxmRubyPrivate,"In PageTableBuffer.cc m_shareEntryPTEData.\n");
		//std::cout<<"In PageTableBuffer.cc m_shareEntryPTEData."<<std::endl;
	    for(iter = m_shareEntryPTEData.begin();iter != m_shareEntryPTEData.end();iter++)  //std::cout<<iter->second<<std::endl;
		DPRINTF(hxmRubyPrivate,"iter->second:%#x.\n",iter->second);
	    return true;
	 }
}
//using for pagetable walker checking trigger
bool
PageTableBuffer::checkPageRequstAddress(PacketPtr pkt,uint32_t id)
{
	//second: |_||_||_|***|_|	|_|    |_|
	//         63 62 61   10	 9	8
	//		             pdp	lock  private					  
	//uint64_t ppnsecond= 0;
	uint64_t address = pkt->getAddr();
	std::map<uint64_t,uint64_t>::iterator iter;
	uint64_t ppnfirst = address & (mask(40) << 12);//4KB
	for(iter = m_shareEntryPTEData.begin();iter != m_shareEntryPTEData.end();iter++)  //std::cout<<iter->second<<std::endl;
	     DPRINTF(hxmRubyPrivate,"in checkPageRequstAddress,iter->second:%#x. and address:%#x.id:%#x.\n",iter->second,address,id);
	iter = m_shareEntryPTEData.find(ppnfirst);
	if(iter !=m_shareEntryPTEData.end()) { //the 4kb page that address belongs to is accessed before
	//check its share or private 
		DPRINTF(hxmRubyPrivate,"in checkRequestAddress find same address.\n");
		   if(((NodeID)(iter->second & 0xff) != id)&&(iter ->second & 0x100))//private,(9th bit  1:private) have different keeper,trigger
		   {
			 // pkt->trigger = true;
			 //  return true;
			 //there will be some problem
			 if(iter ->second & 0x800){
			   assert(iter->second & 0x200);//lock state 
			   iter->second =iter->second & 0xfffffffffffff4ff;//set unlock,share,release
			   pkt->trigger = false;
			   return false;//share
			 }else{
			   pkt->keeper_pkt = 0xff & (iter->second);//set keeper
			   pkt->trigger = true;
			   iter->second = (iter->second)|0xa00;//set lock(private)
			   DPRINTF(hxmRubyPrivate, "checkAddressPTE222222222222222222222222222222222222222setshare.\n");
			   return true;
			 }
		   }else return false;
	}else {//no 4kb page ,don't care
		return true; //ignore
	}
}

//using for 4KB page to check in L1cache
bool
PageTableBuffer::checkAddress(const Address& addr)
{
     //second: |_||_||_|***|_|   |_|    |_|
     //         63 62 61   10     9      8
     //                    pdp   lock  private                     
     uint64_t address = addr.getAddress();
	 std::map<uint64_t,uint64_t>::iterator iter;
	 uint64_t ppnfirst = address & (mask(40) << 12);//4KB
	 iter = m_shareEntryPTEData.find(ppnfirst);
	 if(iter !=m_shareEntryPTEData.end()) { //the 4kb page that address belongs to is accessed before
	    //check its share or private 
	    if(!(iter ->second & 0x100)) return false;//share
	    else return true;
	 }
	 assert(false);
	 return true;
}
//using test pagetable walker
//using for 4KB page to check in L1cache
/*bool
PageTableBuffer::checkAddress(const Address& addr)
{
     //second: |_||_||_|***|_|   |_|    |_|
     //         63 62 61   10     9      8
     //                    pdp   lock  private                     
     uint64_t address = addr.getAddress();
	 std::map<uint64_t,uint64_t>::iterator iter;
	 uint64_t ppnfirst = address & (mask(40) << 12);//4KB
	 iter = m_shareEntryPTEData.find(ppnfirst);
	 if(iter !=m_shareEntryPTEData.end()) { //the 4kb page that address belongs to is accessed before
	    //check its share or private 
	    if(!(iter ->second & 0x100)) return false;//share
	    else return true;
	 }
	 assert(false);
	 return true;
}*/

//there may be some bug,will solve it later
void
PageTableBuffer::checkcacheAddress(const Address& address,DataBlock& dblk,const MachineID& Machinenum)
{
     NodeID num = Machinenum.getNum();
	 uint64_t ppnsecond= 0;
	 uint8_t data[8] = {0};
	 std::map<uint64_t,uint64_t>::iterator iter;
	 Address ppnaddress(address.getppnAddress());
	 memcpy(data,dblk.getData(ppnaddress.getOffset(),8),8);
	 EntryPageTable pte64 = *((uint64_t*)data);
	 uint64_t ppnfirst = ((uint64_t)pte64) & (mask(40) << 12);//4KB
	 if(!pte64.p) return;//pagetable don't exist;
	 iter = m_shareEntryPTEData.find(ppnfirst);
     if(iter !=m_shareEntryPTEData.end()){//have same PTE  
         DPRINTF(hxmRubyPrivate, "PTE000000000000000000000000000000000000000iter->second=%#016x.\n",iter->second);
         if(!(iter ->second & 0x100)){//share
    	   pte64.prv = 0;
    	   pte64.trig = 0;
    	   pte64.interrupt10cpu = 0xff & (iter->second);
    	   DPRINTF(hxmRubyPrivate, "PTE11111111111111111111111111111111111share.\n");
        }else if((NodeID)(iter->second & 0xff) != num)//private,(9th bit	1:private) have different keeper
    	{
    	   pte64.prv = 0;//set share
    	   pte64.interrupt10cpu = 0xff & (iter->second);//set keeper
    	   pte64.trig = 1;
    	   iter->second = (iter->second)&0xfffffffffffffeff;//set share 
    	   DPRINTF(hxmRubyPrivate, "PTE222222222222222222222222222222222222222setshare.\n");
        }else//private
    	{
    	   pte64.prv = 1;
    	   pte64.interrupt10cpu = 0xff & (iter->second &0xff);
    	   pte64.trig = 0;
    	   DPRINTF(hxmRubyPrivate, "PTE333333333333333333333333333333333333333keepprivate.\n");
    	 }
      }else{//no data so it is the first acsess 4KB page
    	   pte64.prv = 1;;
    	   pte64.interrupt10cpu = (uint64_t)num&0xff;
    	   pte64.trig = 0; //don't trigger
    	   DPRINTF(hxmRubyPrivate, "PTE4444444444444444444444444444444444444444444addprivate.\n");
    	   ppnsecond = ppnfirst|0x100;//private,keep
    	   ppnsecond = ppnsecond|((uint64_t)num&0xff);//private,keep
    	   assert(num<0xff);
    	   DPRINTF(hxmRubyPrivate, "PTE In PageTableBuffer.cc Keeper: %#016x.\n",(uint64)(0xff&num));
    	   m_shareEntryPTEData.insert(map<uint64_t,uint64_t>::value_type(ppnfirst,ppnsecond));
    	   DPRINTF(hxmRubyPrivate, "PTE In PageTableBuffer.cc ppnfirst: %#016x.ppnsecond: %#016x\n",(uint64)(ppnfirst),(uint64)(ppnsecond));
      }
	 //write dblk
	 *(uint64_t*)data = pte64;
	 dblk.setData(data,ppnaddress.getOffset(),8);
	 DPRINTF(hxmRubyPrivate, "In PageTableBuffer.cc pte64: %#016x.After\n",(uint64_t)pte64);

}

void 
PageTableBuffer::recordEntryAddrss(const Address& address,DataBlock& dblk,const MachineID& Machinenum,bool pdp)
{
    NodeID num = Machinenum.getNum();
    DPRINTF(hxmRubyPrivate, "Recorded EntryAddrss: %#016x.NodeID is %#016x\n",address.getAddress(),num);
	DPRINTF(hxmRubyPrivate, "Recorded EntryAddrss_pysical: %#016x.NodeID is %#016x\n",address.getppnAddress(),num);
	Address ppnaddress(address.getppnAddress());
	uint8_t data[8] = {0};
	Addr ppnfirst,ppnsecond ;//record the address and flag of keeper,private
	uint8_t shareflag=0;
	uint8_t privateflag=0;
	std::map<uint64_t,uint64_t> map_temprivate;
	std::map<uint64_t,uint64_t>::iterator iter;
	memcpy(data,dblk.getData(ppnaddress.getOffset(),8),8);
	EntryPageTable pte64 = *((uint64_t*)data);
   	DPRINTF(hxmRubyPrivate, "Recorded PageTableEntryhxm: %#016x.data before.\n",(uint64_t)pte64);
	bool flagpdp = pdp && pte64.p && pte64.ps;
	bool flagpte = (!pdp) && pte64.p;
    DPRINTF(hxmRubyPrivate, "In PageTableBuffer.cc m_shareEntryPDPData.\n");
    for(iter = m_shareEntryPDPData.begin();iter != m_shareEntryPDPData.end();iter++)
		DPRINTF(hxmRubyPrivate, "%#016x.\n",iter->second);
		 //std::cout<<iter->second<<std::endl;	 	
	 	 //std::cout<<"In PageTableBuffer.cc m_shareEntryPDPData."<<std::endl;
    DPRINTF(hxmRubyPrivate, "In PageTableBuffer.cc m_shareEntryPTEData.\n");
	for(iter = m_shareEntryPTEData.begin();iter != m_shareEntryPTEData.end();iter++)
		DPRINTF(hxmRubyPrivate, "%#016x.\n",iter->second);
		 //std::cout<<iter->second<<std::endl;
    if(flagpdp){//2M page
  		ppnfirst = (uint64_t)pte64 & (mask(31) << 21);//get physical address number
  		iter = m_shareEntryPDPData.find(ppnfirst);
		if(iter !=m_shareEntryPDPData.end()){//have data
			DPRINTF(hxmRubyPrivate, "PDP000000000000000000000000000000000000000iter->second=%#016x.\n",iter->second);
			if(!(iter ->second & 0x100)){//share
				pte64.prv = 0;
				pte64.trig = 0;
				pte64.interrupt10cpu = 0xf & (iter->second);
				DPRINTF(hxmRubyPrivate, "PDP11111111111111111111111111111111111share.\n");
			}
			else if((NodeID)(iter->second & 0xff) != num)//private,(9th bit  1:private) have defferent keeper
			{
				pte64.prv = 0;//set share
				pte64.interrupt10cpu = 0xf & (iter->second);//set keeper
				pte64.trig = 1;
				iter->second = (iter->second)&0xfffffffffffffeff;//set share 
				DPRINTF(hxmRubyPrivate, "PDP222222222222222222222222222222222222222setshare.\n");
			}
			else//private
			{
				pte64.prv = 1;
				pte64.interrupt10cpu = iter->second &0xff;
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
					pte64.interrupt10cpu = 0x0;
					int numcpu = 0;
					for(map<uint64_t,uint64_t>::iterator i =map_temprivate.begin();i != map_temprivate.end();i++){
						if((NodeID)(iter->second & 0xff) != num){
							//share,trigger,record data
							numcpu++;
							if(numcpu >1) assert(pte64.interrupt10cpu == (0xff & (iter->second)));
							pte64.interrupt10cpu = 0xff & (iter->second);
							//pte64.interrupt10cpu = pte64.interrupt10cpu |(0x1<<(0x3 & (iter->second)));
							DPRINTF(hxmRubyPrivate, "PDP In PageTableBuffer.cc PTE iter->second interruptCPU %#016x\n",iter->second);
						}
					}
					map_temprivate.clear();
				 //insert 2M page map
				   ppnsecond = ppnfirst & 0xfffffffffffffeff;//set share
				 //set keeper core current CPU,so mayby some TLB will keep deffently but no deffence
				   ppnsecond = ppnsecond|((uint64_t)num&0xff); 
				   DPRINTF(hxmRubyPrivate, "PDP In PageTableBuffer.cc Keeper: %#016x.\n",(uint64)(0xff&num));
				   m_shareEntryPDPData.insert(map<uint64_t,uint64_t>::value_type(ppnfirst,ppnsecond));
				   DPRINTF(hxmRubyPrivate, "PDP In PageTableBuffer.cc ppnfirst: %#016x.ppnsecond: %#016x\n",(uint64)(ppnfirst),(uint64)(ppnsecond));
				}else if(shareflag){
				 //only shareflag = 1;there are only 4KB share page 
				 //no 2MB page but exist 4KB share page so erase 4KB (it has done before) 
				 //set share 2M page share,no trigger,set keeper current cpu(L1cahce number)
				   pte64.prv = 0;
				   pte64.trig = 0;
				   pte64.interrupt10cpu = (uint64_t)num&0xff; 
				 //insert 2M page map
				   ppnsecond = ppnfirst & 0xfffffffffffffeff;//set share
				 //set keeper core current CPU,so mayby some TLB will keep deffently but no deffence
				   ppnsecond = ppnsecond|((uint64_t)num&0xff); 
				   DPRINTF(hxmRubyPrivate, "PDP In PageTableBuffer.cc Keeper: %#016x.\n",(uint64)(0xff&num));
				   m_shareEntryPDPData.insert(map<uint64_t,uint64_t>::value_type(ppnfirst,ppnsecond));
				   DPRINTF(hxmRubyPrivate, "PDP In PageTableBuffer.cc ppnfirst: %#016x.ppnsecond: %#016x\n",(uint64)(ppnfirst),(uint64)(ppnsecond));
				}else if(privateflag){
				//only privateflag = 1;there are only 4KB private page 
				//so should check the keeper,if it is different,page should set share,otherwise set private.
				//first to check keeper. 
				    pte64.prv =1;
				    pte64.trig = 0;
					int numcpu2 = 0;
					for(map<uint64_t,uint64_t>::iterator i =map_temprivate.begin();i != map_temprivate.end();i++){
						if((NodeID)(iter->second & 0xff) != num){
							//share,trigger,record data
							numcpu2++;
							if(numcpu2 >1) assert(pte64.interrupt10cpu == (0xff & (iter->second)));						
							pte64.prv = 0;
							pte64.trig = 1;
							pte64.interrupt10cpu = 0xff & (iter->second);	
							//pte64.interrupt10cpu = pte64.interrupt10cpu |(0x1<<(0x3 & (iter->second)));
							DPRINTF(hxmRubyPrivate, "PDP many private PTE In PageTableBuffer.cc PTE iter->second interruptCPU %#016x\n",iter->second);
						}
					}
				   map_temprivate.clear();
				   //set keeper core current CPU
				   if(pte64.prv) ppnsecond = ppnfirst|0x100;//set privateppnsecond = ppnfirst|0x100;//private,keep
				   else ppnsecond = ppnfirst & 0xfffffffffffffeff;
				   ppnsecond = ppnsecond|((uint64_t)num&0xff); 
				   DPRINTF(hxmRubyPrivate, "PDP In PageTableBuffer.cc Keeper: %#016x.\n",(uint64)(0xff&num));
				   m_shareEntryPDPData.insert(map<uint64_t,uint64_t>::value_type(ppnfirst,ppnsecond));
				   DPRINTF(hxmRubyPrivate, "PDP In PageTableBuffer.cc ppnfirst: %#016x.ppnsecond: %#016x\n",(uint64)(ppnfirst),(uint64)(ppnsecond));
				}else {
				 //shareflag = 0,privateflag = 0,so it is a nem 2M page
				 //no 2MB page ,no 4KB page 
				 //set 2M page private,set keeper current cpu(L1cahce number),no trigger,put into 2M map
				   pte64.prv = 1;
				   pte64.trig = 0;
				   pte64.interrupt10cpu = (uint64_t)num&0xff;
				 //insert 2M page map
				   ppnsecond = ppnfirst |0x100;//set private
				   ppnsecond = ppnsecond|((uint64_t)num&0xff);
				   assert(num <= 0xff);
				   DPRINTF(hxmRubyPrivate, "PDP In PageTableBuffer.cc Keeper: %#016x.\n",(uint64)(0xff&num));
				   m_shareEntryPDPData.insert(map<uint64_t,uint64_t>::value_type(ppnfirst,ppnsecond));
				   DPRINTF(hxmRubyPrivate, "PDP In PageTableBuffer.cc ppnfirst: %#016x.ppnsecond: %#016x\n",(uint64)(ppnfirst),(uint64)(ppnsecond));
				}

		}
		//write dblk
		*(uint64_t*)data = pte64;
		dblk.setData(data,ppnaddress.getOffset(),8);
		DPRINTF(hxmRubyPrivate, "PDP In PageTableBuffer.cc pte64: %#016x.After\n",(uint64_t)pte64);		
	}else if(flagpte){
		ppnfirst = (uint64_t)pte64 & (mask(31) << 21);//get PTE 31bit to compare with PDP map
  		iter = m_shareEntryPDPData.find(ppnfirst);
		if(iter != m_shareEntryPDPData.end()){//have data
			DPRINTF(hxmRubyPrivate, "PTE000000000000000000000000000000000000000PDPiter->second=%#016x.\n",iter->second);
			if(!(iter ->second & 0x100)){//share
				pte64.prv = 0;
				pte64.trig = 0;
				pte64.interrupt10cpu = 0xff& (iter->second);
				DPRINTF(hxmRubyPrivate, "PTE11111111111111111111111111111111111PDPshare.\n");
			}
			else if((NodeID)(iter->second & 0xff) != num)//private,(9th bit  1:private) have different keeper
			{
				pte64.prv = 0;//set share
				pte64.interrupt10cpu = 0xff & (iter->second);//set keeper
				pte64.trig = 1;
				iter->second = (iter->second)&0xfffffffffffffeff;//set share 
				DPRINTF(hxmRubyPrivate, "PTE222222222222222222222222222222222222222PDPsetshare.\n");
			}
			else//private
			{
				pte64.prv = 1;
				pte64.interrupt10cpu = 0xff & (iter->second);
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
				  pte64.interrupt10cpu = 0xff & (iter->second);
				  DPRINTF(hxmRubyPrivate, "PTE11111111111111111111111111111111111share.\n");
			  }
			  else if((NodeID)(iter->second & 0xff) != num)//private,(9th bit  1:private) have different keeper
			  {
				  pte64.prv = 0;//set share
				  pte64.interrupt10cpu = 0xff & (iter->second);//set keeper
				  pte64.trig = 1;
				  iter->second = (iter->second)&0xfffffffffffffeff;//set share 
				  DPRINTF(hxmRubyPrivate, "PTE222222222222222222222222222222222222222setshare.\n");
			  }
			  else//private
			  {
				  pte64.prv = 1;
				  pte64.interrupt10cpu = 0xff & (iter->second &0xff);
				  pte64.trig = 0;
				  DPRINTF(hxmRubyPrivate, "PTE333333333333333333333333333333333333333keepprivate.\n");
			  }
		  }else{//no data so it is the first acsess 4KB page
			  pte64.prv = 1;;
			  pte64.interrupt10cpu = (uint64_t)num&0xff;
			  pte64.trig = 0; //don't trigger
			  DPRINTF(hxmRubyPrivate, "PTE4444444444444444444444444444444444444444444addprivate.\n");
			  ppnsecond = ppnfirst|0x100;//private,keep
			  ppnsecond = ppnsecond|((uint64_t)num&0xff);//private,keep
			  assert(num<0xff);
			  DPRINTF(hxmRubyPrivate, "PTE In PageTableBuffer.cc Keeper: %#016x.\n",(uint64)(0xff&num));
			  m_shareEntryPTEData.insert(map<uint64_t,uint64_t>::value_type(ppnfirst,ppnsecond));
			  DPRINTF(hxmRubyPrivate, "PTE In PageTableBuffer.cc ppnfirst: %#016x.ppnsecond: %#016x\n",(uint64)(ppnfirst),(uint64)(ppnsecond));
		  }
		}
		//write dblk
		*(uint64_t*)data = pte64;
		dblk.setData(data,ppnaddress.getOffset(),8);
		DPRINTF(hxmRubyPrivate, "In PageTableBuffer.cc pte64: %#016x.After\n",(uint64_t)pte64);
	}
	DPRINTF(hxmRubyPrivate, "In PageTableBuffer.cc page fault pte64: %#016x.After\n",(uint64_t)pte64);
}

//@hxm*****************************************************************
bool 
PageTableBuffer::checkEntryAddrss(const Address& address,MachineID& Machinenum)
{
     uint64_t ppnfirst = address.getAddress();
	 std::map<uint64_t,uint64_t>::iterator iter;
	 ppnfirst = (uint64_t)ppnfirst & (mask(40) << 12);
	 iter = m_shareEntryPTEData.find(ppnfirst);
	 if(iter !=m_shareEntryPTEData.end()) {
        if(!(iter ->second & 0x100)) return false;//share
        else{
			Machinenum.num = (iter->second)&0xff;
			Machinenum.type = MachineType_L1Cache;
			std::cout<<"in PageTableBuffer.cc find EntryAddrss."<<std::endl;
			return true;//private
		}
	 }
	 ppnfirst = (uint64_t)ppnfirst & (mask(31) << 21);//get physical address，maybe 2M and 4K ->as 2M
     iter = m_shareEntryPDPData.find(ppnfirst);
	 if(iter !=m_shareEntryPDPData.end()){
	   if(!(iter ->second & 0x100)) return false;//share
	   else{
		Machinenum.num = (iter->second)&0xff;
		Machinenum.type = MachineType_L1Cache;
		std::cout<<"in PageTableBuffer.cc find EntryAddrss."<<std::endl;
	   	return true;
	   }
	  }
	 std::cout<<"In PageTableBuffer.cc m_shareEntryPDPData."<<std::endl;
	 for(iter = m_shareEntryPDPData.begin();iter != m_shareEntryPDPData.end();iter++)
		 std::cout<<iter->second<<std::endl;	 	
	 	 std::cout<<"In PageTableBuffer.cc m_shareEntryPTEData."<<std::endl;
	 for(iter = m_shareEntryPTEData.begin();iter != m_shareEntryPTEData.end();iter++)
		 std::cout<<iter->second<<std::endl;
	 std::cout<<"In PageTableBuffer.cc no pagetable address="<<address.getAddress()<<",so maybe pte,pde address,maybe something wrong."<<std::endl;
	 //assert(false);
	 return false;
}
//@hxm*****************************************************************
