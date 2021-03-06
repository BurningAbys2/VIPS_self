/*
 * Copyright (c) 2012-2013 ARM Limited
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
 * Copyright (c) 2008 The Hewlett-Packard Development Company
 * All rights reserved.
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
 * Authors: Gabe Black
 */

#include <memory>
#include <iomanip>
#include "arch/x86/regs/apic.hh"
#include "arch/x86/interrupts.hh"
#include "arch/x86/intmessage.hh"
#include "cpu/thread_context.hh"
#include "cpu/base.hh"
#include "debug/LocalApic.hh"
#include "debug/LocalApichxm.hh"
#include "dev/x86/i82094aa.hh"
#include "dev/x86/pc.hh"
#include "dev/x86/south_bridge.hh"
#include "mem/packet_access.hh"
#include "sim/system.hh"
#include "sim/full_system.hh"


int
divideFromConf(uint32_t conf)
{
    // This figures out what division we want from the division configuration
    // register in the local APIC. The encoding is a little odd but it can
    // be deciphered fairly easily.
    DPRINTF(LocalApichxm,"enter divideFromConf\n");
    int shift = ((conf & 0x8) >> 1) | (conf & 0x3);
    shift = (shift + 1) % 8;
    DPRINTF(LocalApichxm,"leave divideFromconf\n");
    return 1 << shift;
}

namespace X86ISA
{

ApicRegIndex
decodeAddr(Addr paddr)
{
    DPRINTF(LocalApichxm,"enter X86ISA ApicRegIndex decodeAddr(Addr paddr)\n");
    ApicRegIndex regNum;
    paddr &= ~mask(3);
    switch (paddr)
    {
      case 0x20:
        regNum = APIC_ID;
        break;
      case 0x30:
        regNum = APIC_VERSION;
        break;
      case 0x80:
        regNum = APIC_TASK_PRIORITY;
        break;
      case 0x90:
        regNum = APIC_ARBITRATION_PRIORITY;
        break;
      case 0xA0:
        regNum = APIC_PROCESSOR_PRIORITY;
        break;
      case 0xB0:
        regNum = APIC_EOI;
        break;
      case 0xD0:
        regNum = APIC_LOGICAL_DESTINATION;
        break;
      case 0xE0:
        regNum = APIC_DESTINATION_FORMAT;
        break;
      case 0xF0:
        regNum = APIC_SPURIOUS_INTERRUPT_VECTOR;
        break;
      case 0x100:
      case 0x108:
      case 0x110:
      case 0x118:
      case 0x120:
      case 0x128:
      case 0x130:
      case 0x138:
      case 0x140:
      case 0x148:
      case 0x150:
      case 0x158:
      case 0x160:
      case 0x168:
      case 0x170:
      case 0x178:
        regNum = APIC_IN_SERVICE((paddr - 0x100) / 0x8);
        break;
      case 0x180:
      case 0x188:
      case 0x190:
      case 0x198:
      case 0x1A0:
      case 0x1A8:
      case 0x1B0:
      case 0x1B8:
      case 0x1C0:
      case 0x1C8:
      case 0x1D0:
      case 0x1D8:
      case 0x1E0:
      case 0x1E8:
      case 0x1F0:
      case 0x1F8:
        regNum = APIC_TRIGGER_MODE((paddr - 0x180) / 0x8);
        break;
      case 0x200:
      case 0x208:
      case 0x210:
      case 0x218:
      case 0x220:
      case 0x228:
      case 0x230:
      case 0x238:
      case 0x240:
      case 0x248:
      case 0x250:
      case 0x258:
      case 0x260:
      case 0x268:
      case 0x270:
      case 0x278:
        regNum = APIC_INTERRUPT_REQUEST((paddr - 0x200) / 0x8);
        break;
      case 0x280:
        regNum = APIC_ERROR_STATUS;
        break;
      case 0x300:
        regNum = APIC_INTERRUPT_COMMAND_LOW;
        break;
      case 0x310:
        regNum = APIC_INTERRUPT_COMMAND_HIGH;
        break;
      case 0x320:
        regNum = APIC_LVT_TIMER;
        break;
      case 0x330:
        regNum = APIC_LVT_THERMAL_SENSOR;
        break;
      case 0x340:
        regNum = APIC_LVT_PERFORMANCE_MONITORING_COUNTERS;
        break;
      case 0x350:
        regNum = APIC_LVT_LINT0;
        break;
      case 0x360:
        regNum = APIC_LVT_LINT1;
        break;
      case 0x370:
        regNum = APIC_LVT_ERROR;
        break;
      case 0x380:
        regNum = APIC_INITIAL_COUNT;
        break;
      case 0x390:
        regNum = APIC_CURRENT_COUNT;
        break;
      case 0x3E0:
        regNum = APIC_DIVIDE_CONFIGURATION;
        break;
      default:
        // A reserved register field.
        panic("Accessed reserved register field %#x.\n", paddr);
        break;
    }
    DPRINTF(LocalApichxm,"leave X86ISA ApicRegIndex decodeAddr(Addr paddr)\n");
    return regNum;
}
}

Tick
X86ISA::Interrupts::read(PacketPtr pkt)
{
    DPRINTF(LocalApichxm,"enter Tick X86ISA::Interrupts::read(PacketPtr pkt)\n");
    Addr offset = pkt->getAddr() - pioAddr;
    //Make sure we're at least only accessing one register.
    if ((offset & ~mask(3)) != ((offset + pkt->getSize()) & ~mask(3)))
        panic("Accessed more than one register at a time in the APIC!\n");
    ApicRegIndex reg = decodeAddr(offset);
    uint32_t val = htog(readReg(reg));
    DPRINTF(LocalApic,
            "Reading Local APIC register %d at offset %#x as %#x.\n",
            reg, offset, val);
    pkt->setData(((uint8_t *)&val) + (offset & mask(3)));
    pkt->makeAtomicResponse();
    DPRINTF(LocalApichxm,"leave Tick X86ISA::Interrupts::read(PacketPtr pkt)\n");
    return pioDelay;
}

Tick
X86ISA::Interrupts::write(PacketPtr pkt)
{
    DPRINTF(LocalApichxm,"enter Tick X86ISA::Interrupts::write(PacketPtr pkt)\n");
    Addr offset = pkt->getAddr() - pioAddr;
    //Make sure we're at least only accessing one register.
    if ((offset & ~mask(3)) != ((offset + pkt->getSize()) & ~mask(3)))
        panic("Accessed more than one register at a time in the APIC!\n");
    ApicRegIndex reg = decodeAddr(offset);
    uint32_t val = regs[reg];
    pkt->writeData(((uint8_t *)&val) + (offset & mask(3)));
    DPRINTF(LocalApic,
            "Writing Local APIC register %d at offset %#x as %#x.\n",
            reg, offset, gtoh(val));
    setReg(reg, gtoh(val));
    pkt->makeAtomicResponse();
    DPRINTF(LocalApichxm,"leave Tick X86ISA::Interrupts::write(PacketPtr pkt)\n");
    return pioDelay;
}
void
X86ISA::Interrupts::requestInterrupt(uint8_t vector,
        uint8_t deliveryMode, bool level)
{
    /*
     * Fixed and lowest-priority delivery mode interrupts are handled
     * using the IRR/ISR registers, checking against the TPR, etc.
     * The SMI, NMI, ExtInt, INIT, etc interrupts go straight through.
     */
    DPRINTF(LocalApichxm,"enter void X86ISA::Interrupts::requestInerrupt()\n");
    if (deliveryMode == DeliveryMode::Fixed ||
            deliveryMode == DeliveryMode::LowestPriority) {
        DPRINTF(LocalApic, "Interrupt is an %s.\n",
                DeliveryMode::names[deliveryMode]);
        // Queue up the interrupt in the IRR.
        if (vector > IRRV)
            IRRV = vector;
        if (!getRegArrayBit(APIC_INTERRUPT_REQUEST_BASE, vector)) {
            setRegArrayBit(APIC_INTERRUPT_REQUEST_BASE, vector);
            if (level) {
                setRegArrayBit(APIC_TRIGGER_MODE_BASE, vector);
            } else {
                clearRegArrayBit(APIC_TRIGGER_MODE_BASE, vector);
            }
        }
    } else if (!DeliveryMode::isReserved(deliveryMode)) {
        DPRINTF(LocalApic, "Interrupt is an %s.\n",
                DeliveryMode::names[deliveryMode]);
        if (deliveryMode == DeliveryMode::SMI && !pendingSmi) {
            pendingUnmaskableInt = pendingSmi = true;
            smiVector = vector;
        } else if (deliveryMode == DeliveryMode::NMI && !pendingNmi) {
            pendingUnmaskableInt = pendingNmi = true;
            nmiVector = vector;
        } else if (deliveryMode == DeliveryMode::ExtInt && !pendingExtInt) {
            pendingExtInt = true;
            extIntVector = vector;
        } else if (deliveryMode == DeliveryMode::INIT && !pendingInit) {
            pendingUnmaskableInt = pendingInit = true;
            initVector = vector;
        } else if (deliveryMode == DeliveryMode::SIPI &&
                !pendingStartup && !startedUp) {
            pendingUnmaskableInt = pendingStartup = true;
            startupVector = vector;
        }
    }
    if (FullSystem)
        cpu->wakeup();
    DPRINTF(LocalApichxm,"leave Tick X86ISA::Interrupts::requestInterrupt ()\n");
}


void
X86ISA::Interrupts::setCPU(BaseCPU * newCPU)
{
    DPRINTF(LocalApichxm,"enter void X86ISA::Interrupts::setCPU()\n");
    assert(newCPU);
    if (cpu != NULL && cpu->cpuId() != newCPU->cpuId()) {
        panic("Local APICs can't be moved between CPUs"
                " with different IDs.\n");
    }
    cpu = newCPU;
    initialApicId = cpu->cpuId();
    std::cout<<"initialApicId="<<initialApicId<<std::endl;
    regs[APIC_ID] = (initialApicId << 24);
    pioAddr = x86LocalAPICAddress(initialApicId, 0);
    DPRINTF(LocalApichxm,"leave void X86ISA::Interrupts::setCPU()\n");
}


void
X86ISA::Interrupts::init()
{
    //
    // The local apic must register its address ranges on both its pio
    // port via the basicpiodevice(piodevice) init() function and its
    // int port that it inherited from IntDevice.  Note IntDevice is
    // not a SimObject itself.
    //
    DPRINTF(LocalApichxm,"enter void X86ISA::Interrupts::init()\n");
    BasicPioDevice::init();
    IntDevice::init();

    // the slave port has a range so inform the connected master
    intSlavePort.sendRangeChange();
    DPRINTF(LocalApichxm,"leave void X86ISA::Interrupts::int()\n");

}


Tick
X86ISA::Interrupts::recvMessage(PacketPtr pkt)
{
    DPRINTF(LocalApichxm,"enter void X86ISA::recvMessage() and pkt-getAddr = %x and x86InterruptAddress = % x \n",pkt->getAddr(),x86InterruptAddress(initialApicId,0));    
    Addr offset = pkt->getAddr() - x86InterruptAddress(initialApicId, 0);
    assert(pkt->cmd == MemCmd::MessageReq);
    switch(offset)
    {
	  DPRINTF(LocalApichxm,"current ApicId = %d\n",getInitialApicId()); 
      case 0:
        {
            if(pkt->getSize() == sizeof(TriggerIntMessage)){
		    TriggerIntMessage message = pkt->get<TriggerIntMessage>();
		    DPRINTF(LocalApichxm,
				    "Got Trigger Interrupt message with vector %#x.\n",
				    message.vector);
		    requestInterrupt(message.vector,message.deliveryMode, message.trigger);
	        }else{
				//@hxm*****************************************************************************		
				//only used by timing cpu fetch
				assert(pkt->getSize()==8);
				recvAddr = pkt->get<Addr>();
				switch(recvAddr){
					case 1:
						std::cout<<"interrupt recvMessage cpu triggerFetch"<<std::endl;
						cpu->triggerFetch();
						break;
					case 2:
						std::cout<<"interrupt recvMessage cpu triggerWritePacket"<<std::endl;
						cpu->triggerWritePacket();
						break;
					case 3:
						std::cout<<"interrupt recvMessage cpu triggerReadPacket"<<std::endl;
						cpu->triggerReadPacket();
						break;
					case 4:
					    std::cout<<"interrupt recvMessage cpu triggerITB"<<std::endl;
						cpu->triggerITB();
						break;
					case 5:
						std::cout<<"interrupt recvMessage cpu triggerDTB write"<<std::endl;
						cpu->triggerDTBwrite();
						break;
					case 6:
						std::cout<<"interrupt recvMessage cpu triggerDTB read"<<std::endl;
						cpu->triggerDTBread();
						break;

					default:
						if(pendingTransition == false){
							pendingNum = 0;
							triggerCPU = (uint8_t)(recvAddr & 0xff);//low 8 bit deserve cpu_id
							triggerUnit = (uint8_t)((recvAddr & 0xf00)>>8);//8~11bits decide cpu_unit mybe icache0/dcache(read2,write1)  
							std::cout<<"in the interrupt.cc recvAddr =0x "<<std::hex<<recvAddr<<std::endl;
							triggerAddr = recvAddr & (mask(40)<<12);
							std::cout<<"in the interrupt.cc triggerAddr =0x "<<std::hex<<triggerAddr<<std::endl;
							DPRINTF(LocalApichxm, "Got Trigger Interrupt message with triggerAddr= %#x and triggerCPU= %d.\n",triggerAddr,triggerCPU);
							if (FullSystem) cpu->wakeup();
							pendingTransition = true; 
						}else{
						    pendingNum++;
							assert(pendingNum <16);
                            pendingAddr.push(recvAddr);
							assert(pendingNum == pendingAddr.size());
						}
						break;
				}			
				//@hxm*****************************************************************************
			}
	   }
	break;
      default:
	panic("Local apic got unknown interrupt message at offset %#x.\n",
			offset);
	break;
    }
    pkt->makeAtomicResponse();
    DPRINTF(LocalApichxm,"leave void X86ISA::recvMessage()\n"); 
    return pioDelay;
}


Tick
X86ISA::Interrupts::recvResponse(PacketPtr pkt)
{
    DPRINTF(LocalApichxm,"enter void X86ISA::Interrupts::recvResponse()\n"); 
    assert(!pkt->isError());
    assert(pkt->cmd == MemCmd::MessageResp);
	if(pkt->getSize()==sizeof(TriggerIntMessage)){
		    if (--pendingIPIs == 0) {
		        InterruptCommandRegLow low = regs[APIC_INTERRUPT_COMMAND_LOW];
		        // Record that the ICR is now idle.
		        low.deliveryStatus = 0;
		        regs[APIC_INTERRUPT_COMMAND_LOW] = low;
				DPRINTF(LocalApic, "ICR is now idle.\n");
		    }
		}
    DPRINTF(LocalApichxm,"leave void X86ISA::Interrupts::recvResponse()\n"); 
    return 0;
}


AddrRangeList
X86ISA::Interrupts::getIntAddrRange() const
{
    DPRINTF(LocalApichxm,"enter AddrRangeList X86ISA::Interrupts::getIntAddrRange()\n"); 
    AddrRangeList ranges;
    ranges.push_back(RangeEx(x86InterruptAddress(initialApicId, 0),
                             x86InterruptAddress(initialApicId, 0) +
                             PhysAddrAPICRangeSize));
    DPRINTF(LocalApichxm,"leave AddrRangeList X86ISA::Interrupts::getIntAddrRange()\n"); 
    return ranges;
}


uint32_t
X86ISA::Interrupts::readReg(ApicRegIndex reg)
{
    DPRINTF(LocalApichxm,"enter X86ISA::Interrupts::readReg()\n"); 
    if (reg >= APIC_TRIGGER_MODE(0) &&
            reg <= APIC_TRIGGER_MODE(15)) {
        panic("Local APIC Trigger Mode registers are unimplemented.\n");
    }
    switch (reg) {
      case APIC_ARBITRATION_PRIORITY:
        panic("Local APIC Arbitration Priority register unimplemented.\n");
        break;
      case APIC_PROCESSOR_PRIORITY:
        panic("Local APIC Processor Priority register unimplemented.\n");
        break;
      case APIC_ERROR_STATUS:
        regs[APIC_INTERNAL_STATE] &= ~ULL(0x1);
        break;
      case APIC_CURRENT_COUNT:
        {
            if (apicTimerEvent.scheduled()) {
                // Compute how many m5 ticks happen per count.
                uint64_t ticksPerCount = clockPeriod() *
                    divideFromConf(regs[APIC_DIVIDE_CONFIGURATION]);
                // Compute how many m5 ticks are left.
                uint64_t val = apicTimerEvent.when() - curTick();
                // Turn that into a count.
                val = (val + ticksPerCount - 1) / ticksPerCount;
                return val;
            } else {
                return 0;
            }
        }
      default:
        break;
    }
    DPRINTF(LocalApichxm,"leave X86ISA::Interrupts::readReg()\n"); 
    return regs[reg];
}

void 
X86ISA::Interrupts::sendTrigger(uint8_t keep,Addr paddr)
{
     std::cout<<"in interrupt.cc sendTrigger paddr =0x"<<std::hex<<paddr<<std::endl;
     std::cout<<"in interrupt.cc sendTrigger keep =0x"<<std::hex<<(int)keep<<std::endl;
	 assert(sizeof(paddr) == 8);
     intMasterPort.sendTriggerAddr(keep, paddr,sys->isTimingMode());
	 return;
}

void
X86ISA::Interrupts::setReg(ApicRegIndex reg, uint32_t val)
{
    DPRINTF(LocalApichxm,"enter void X86ISA::Interrupts::setReg()\n"); 
    uint32_t newVal = val;
    if (reg >= APIC_IN_SERVICE(0) &&
            reg <= APIC_IN_SERVICE(15)) {
        panic("Local APIC In-Service registers are unimplemented.\n");
    }
    if (reg >= APIC_TRIGGER_MODE(0) &&
            reg <= APIC_TRIGGER_MODE(15)) {
        panic("Local APIC Trigger Mode registers are unimplemented.\n");
    }
    if (reg >= APIC_INTERRUPT_REQUEST(0) &&
            reg <= APIC_INTERRUPT_REQUEST(15)) {
        panic("Local APIC Interrupt Request registers "
                "are unimplemented.\n");
    }
    switch (reg) {
      case APIC_ID:
        newVal = val & 0xFF;
        break;
      case APIC_VERSION:
        // The Local APIC Version register is read only.
        return;
      case APIC_TASK_PRIORITY:
        newVal = val & 0xFF;
        break;
      case APIC_ARBITRATION_PRIORITY:
        panic("Local APIC Arbitration Priority register unimplemented.\n");
        break;
      case APIC_PROCESSOR_PRIORITY:
        panic("Local APIC Processor Priority register unimplemented.\n");
        break;
      case APIC_EOI:
        // Remove the interrupt that just completed from the local apic state.
        clearRegArrayBit(APIC_IN_SERVICE_BASE, ISRV);
        updateISRV();
        return;
      case APIC_LOGICAL_DESTINATION:
        newVal = val & 0xFF000000;
        break;
      case APIC_DESTINATION_FORMAT:
        newVal = val | 0x0FFFFFFF;
        break;
      case APIC_SPURIOUS_INTERRUPT_VECTOR:
        regs[APIC_INTERNAL_STATE] &= ~ULL(1 << 1);
        regs[APIC_INTERNAL_STATE] |= val & (1 << 8);
        if (val & (1 << 9))
            warn("Focus processor checking not implemented.\n");
        break;
      case APIC_ERROR_STATUS:
        {
            if (regs[APIC_INTERNAL_STATE] & 0x1) {
                regs[APIC_INTERNAL_STATE] &= ~ULL(0x1);
                newVal = 0;
            } else {
                regs[APIC_INTERNAL_STATE] |= ULL(0x1);
                return;
            }

        }
        break;
      case APIC_INTERRUPT_COMMAND_LOW:
        {
            InterruptCommandRegLow low = regs[APIC_INTERRUPT_COMMAND_LOW];
            // Check if we're already sending an IPI.
            if (low.deliveryStatus) {
                newVal = low;
                break;
            }
            low = val;
            InterruptCommandRegHigh high = regs[APIC_INTERRUPT_COMMAND_HIGH];
			DPRINTF(LocalApichxm,"1:APIC_INTERRUPT_COMMAND_HIGH:0x%x\n",high); 
			DPRINTF(LocalApichxm,"2:APIC_INTERRUPT_COMMAND_LOW:0x%x\n",low); 			
			//std::cout<<"the high="<<high<<std::endl;
			//std::cout<<"the low="<<low<<std::endl;
			TriggerIntMessage message = 0;
            message.destination = high.destination;
            message.vector = low.vector;
            message.deliveryMode = low.deliveryMode;
            message.destMode = low.destMode;
            message.level = low.level;
            message.trigger = low.trigger;
            ApicList apics;
            int numContexts = sys->numContexts();
            switch (low.destShorthand) {
              case 0:
                if (message.deliveryMode == DeliveryMode::LowestPriority) {
                    panic("Lowest priority delivery mode "
                            "IPIs aren't implemented.\n");
                }
                if (message.destMode == 1) {
		
                    int dest = message.destination;
                    hack_once("Assuming logical destinations are 1 << id.\n");
                    for (int i = 0; i < numContexts; i++) {
                        if (dest & 0x1)
                            apics.push_back(i);
                        dest = dest >> 1;
                    }
                } else {
					DPRINTF(LocalApichxm,"3:enter destMode = Physical\n"); 
                    if (message.destination == 0xFF) {
                        for (int i = 0; i < numContexts; i++) {
                            if (i == initialApicId) {
                                requestInterrupt(message.vector,
                                        message.deliveryMode, message.trigger);
                            } else {
                                apics.push_back(i);
                            }
                        }
                    } else {
                        if (message.destination == initialApicId) {
                            requestInterrupt(message.vector,
                                    message.deliveryMode, message.trigger);
                        } else {
							DPRINTF(LocalApichxm,"4:enter the apics.push_back(),message.destination = %x and initialApicId= %x \n",message.destination,initialApicId); 
                            apics.push_back(message.destination);
                        }
                    }
                }
                break;
              case 1:
                newVal = val;
                requestInterrupt(message.vector,
                        message.deliveryMode, message.trigger);
                break;
              case 2:
                requestInterrupt(message.vector,
                        message.deliveryMode, message.trigger);
                // Fall through
              case 3:
                {
                    for (int i = 0; i < numContexts; i++) {
                        if (i != initialApicId) {
                            apics.push_back(i);
                        }
                    }
                }
                break;
            }
            // Record that an IPI is being sent if one actually is.
            if (apics.size()) {
                low.deliveryStatus = 1;
                pendingIPIs += apics.size();
				DPRINTF(LocalApichxm,"5:apics.size=:%d\n the IPIs message = %08x\n",apics.size(),message); 
                //std::cout<<"the apics.size="<<apics.size()<<std::endl;
				//std::cout<<"the message="<<message<<std::endl;
		}
            regs[APIC_INTERRUPT_COMMAND_LOW] = low;
			DPRINTF(LocalApichxm,"6:enter sendMessage\n"); 
            intMasterPort.sendMessage(apics, message, sys->isTimingMode());
			DPRINTF(LocalApichxm,"7:leave sendMessage\n"); 
            newVal = regs[APIC_INTERRUPT_COMMAND_LOW];
        }
        break;
      case APIC_LVT_TIMER:
      case APIC_LVT_THERMAL_SENSOR:
      case APIC_LVT_PERFORMANCE_MONITORING_COUNTERS:
      case APIC_LVT_LINT0:
      case APIC_LVT_LINT1:
      case APIC_LVT_ERROR:
        {
            uint64_t readOnlyMask = (1 << 12) | (1 << 14);
            newVal = (val & ~readOnlyMask) |
                     (regs[reg] & readOnlyMask);
        }
        break;
      case APIC_INITIAL_COUNT:
        {
            newVal = bits(val, 31, 0);
            // Compute how many timer ticks we're being programmed for.
            uint64_t newCount = newVal *
                (divideFromConf(regs[APIC_DIVIDE_CONFIGURATION]));
            // Schedule on the edge of the next tick plus the new count.
            Tick offset = curTick() % clockPeriod();
            if (offset) {
                reschedule(apicTimerEvent,
                           curTick() + (newCount + 1) *
                           clockPeriod() - offset, true);
            } else {
                if (newCount)
                    reschedule(apicTimerEvent,
                               curTick() + newCount *
                               clockPeriod(), true);
            }
        }
        break;
      case APIC_CURRENT_COUNT:
        //Local APIC Current Count register is read only.
        return;
      case APIC_DIVIDE_CONFIGURATION:
        newVal = val & 0xB;
        break;
      default:
        break;
    }
    regs[reg] = newVal;
    DPRINTF(LocalApichxm,"8:leave void X86ISA::Interrupts::setReg(%d)=0x%x,val=0x%x\n",reg,newVal,val); 
    return;
}


X86ISA::Interrupts::Interrupts(Params * p)
    : BasicPioDevice(p, PageBytes), IntDevice(this, p->int_latency),
      apicTimerEvent(this),
      pendingSmi(false), smiVector(0),
      pendingNmi(false), nmiVector(0),
      pendingExtInt(false), extIntVector(0),
      pendingInit(false), initVector(0),
      pendingStartup(false), startupVector(0),
      startedUp(false), pendingUnmaskableInt(false),
      pendingIPIs(0), cpu(NULL),
      intSlavePort(name() + ".int_slave", this, this)
{
    DPRINTF(LocalApichxm,"enter X86ISA::Interrupts::Interrupts()\n"); 
    memset(regs, 0, sizeof(regs));
    //Set the local apic DFR to the flat model.
    regs[APIC_DESTINATION_FORMAT] = (uint32_t)(-1);
    ISRV = 0;
    IRRV = 0;
	pendingTransition = false;
	pendingNum =0;
	triggerAddr = 0;
	recvAddr =0;
    DPRINTF(LocalApichxm,"leave X86ISA::Interrupts::Interrupts()\n"); 
}


bool
X86ISA::Interrupts::checkInterrupts(ThreadContext *tc) const
{
   // DPRINTF(LocalApic,"enter Interrupts::checkInterrupts()\n"); 
    RFLAGS rflags = tc->readMiscRegNoEffect(MISCREG_RFLAGS);
    if (pendingUnmaskableInt) {
        DPRINTF(LocalApic, "Reported pending unmaskable interrupt.\n");
        return true;
    }
    //std::cout<<"@hxm rflags.intf = "<<rflags.intf<<std::endl;    
    if (rflags.intf) {
      // std::cout<<"enter rflags.intf\n"<<std::endl;
      //  DPRINTF(LocalApic,"enter rflags.intf=%x.\n",rflags.intf);
        if (pendingExtInt) {
            DPRINTF(LocalApic, "Reported pending external interrupt.\n");
            return true;
        }
        if (IRRV > ISRV && bits(IRRV, 7, 4) >
               bits(regs[APIC_TASK_PRIORITY], 7, 4)) {
            DPRINTF(LocalApic, "Reported pending regular interrupt.\n");
            return true;
        }
    }
   // std::cout <<"rflags.intf =="<<rflags.intf<<"\n"<<std::endl;
   // DPRINTF(LocalApic,"leave Interrupts::checkInterrupts()\n");
    return false;
}

bool
X86ISA::Interrupts::checkInterruptsRaw() const
{
    DPRINTF(LocalApichxm,"enter Interrupts::checkInterruptsRaw()\n");
    return pendingUnmaskableInt || pendingExtInt ||
        (IRRV > ISRV && bits(IRRV, 7, 4) >
         bits(regs[APIC_TASK_PRIORITY], 7, 4));
}

Fault
X86ISA::Interrupts::getInterrupt(ThreadContext *tc)
{
    DPRINTF(LocalApichxm,"enter Interrupts::getInterrupt()\n");
    assert(checkInterrupts(tc));
    // These are all probably fairly uncommon, so we'll make them easier to
    // check for.
    if (pendingUnmaskableInt) {
        if (pendingSmi) {
            DPRINTF(LocalApic, "Generated SMI fault object.\n");
            return std::make_shared<SystemManagementInterrupt>();
        } else if (pendingNmi) {
            DPRINTF(LocalApic, "Generated NMI fault object.\n");
            return std::make_shared<NonMaskableInterrupt>(nmiVector);
        } else if (pendingInit) {
            DPRINTF(LocalApic, "Generated INIT fault object.\n");
            return std::make_shared<InitInterrupt>(initVector);
        } else if (pendingStartup) {
            DPRINTF(LocalApic, "Generating SIPI fault object.\n");
            std::cout<<"Generating SIPI fault object"<<std::endl;
            return std::make_shared<StartupInterrupt>(startupVector);
        } else {
            panic("pendingUnmaskableInt set, but no unmaskable "
                    "ints were pending.\n");
            return NoFault;
        }
    } else if (pendingExtInt) {
        DPRINTF(LocalApic, "Generated external interrupt fault object.\n");
        return std::make_shared<ExternalInterrupt>(extIntVector);
    } else {
        DPRINTF(LocalApic, "Generated regular interrupt fault object.\n");
        // The only thing left are fixed and lowest priority interrupts.
        return std::make_shared<ExternalInterrupt>(IRRV);
    }
}
void
X86ISA::Interrupts::updateIntrInfo(ThreadContext *tc)
{
    DPRINTF(LocalApichxm,"enter Interrupts::updateIntrInfo()\n");
    assert(checkInterrupts(tc));
    if (pendingUnmaskableInt) {
        if (pendingSmi) {
            DPRINTF(LocalApic, "SMI sent to core.\n");
            pendingSmi = false;
        } else if (pendingNmi) {
            DPRINTF(LocalApic, "NMI sent to core.\n");
            pendingNmi = false;
        } else if (pendingInit) {
            DPRINTF(LocalApic, "Init sent to core.\n");
            pendingInit = false;
            startedUp = false;
        } else if (pendingStartup) {
            DPRINTF(LocalApic, "SIPI sent to core.\n");
            pendingStartup = false;
            startedUp = true;
        }
        if (!(pendingSmi || pendingNmi || pendingInit || pendingStartup))
            pendingUnmaskableInt = false;
    } else if (pendingExtInt) {
        pendingExtInt = false;
    } else {
        DPRINTF(LocalApichxm, "Interrupt %d sent to core.\n", IRRV);
        // Mark the interrupt as "in service".
        ISRV = IRRV;
        setRegArrayBit(APIC_IN_SERVICE_BASE, ISRV);
        // Clear it out of the IRR.
        clearRegArrayBit(APIC_INTERRUPT_REQUEST_BASE, IRRV);
        updateIRRV();
    }
}

void
X86ISA::Interrupts::serialize(std::ostream &os)
{
    SERIALIZE_ARRAY(regs, NUM_APIC_REGS);
    SERIALIZE_SCALAR(pendingSmi);
    SERIALIZE_SCALAR(smiVector);
    SERIALIZE_SCALAR(pendingNmi);
    SERIALIZE_SCALAR(nmiVector);
    SERIALIZE_SCALAR(pendingExtInt);
    SERIALIZE_SCALAR(extIntVector);
    SERIALIZE_SCALAR(pendingInit);
    SERIALIZE_SCALAR(initVector);
    SERIALIZE_SCALAR(pendingStartup);
    SERIALIZE_SCALAR(startupVector);
    SERIALIZE_SCALAR(startedUp);
    SERIALIZE_SCALAR(pendingUnmaskableInt);
    SERIALIZE_SCALAR(pendingIPIs);
    SERIALIZE_SCALAR(IRRV);
    SERIALIZE_SCALAR(ISRV);
    bool apicTimerEventScheduled = apicTimerEvent.scheduled();
    SERIALIZE_SCALAR(apicTimerEventScheduled);
    Tick apicTimerEventTick = apicTimerEvent.when();
    SERIALIZE_SCALAR(apicTimerEventTick);
}

void
X86ISA::Interrupts::unserialize(Checkpoint *cp, const std::string &section)
{
    UNSERIALIZE_ARRAY(regs, NUM_APIC_REGS);
    UNSERIALIZE_SCALAR(pendingSmi);
    UNSERIALIZE_SCALAR(smiVector);
    UNSERIALIZE_SCALAR(pendingNmi);
    UNSERIALIZE_SCALAR(nmiVector);
    UNSERIALIZE_SCALAR(pendingExtInt);
    UNSERIALIZE_SCALAR(extIntVector);
    UNSERIALIZE_SCALAR(pendingInit);
    UNSERIALIZE_SCALAR(initVector);
    UNSERIALIZE_SCALAR(pendingStartup);
    UNSERIALIZE_SCALAR(startupVector);
    UNSERIALIZE_SCALAR(startedUp);
    UNSERIALIZE_SCALAR(pendingUnmaskableInt);
    UNSERIALIZE_SCALAR(pendingIPIs);
    UNSERIALIZE_SCALAR(IRRV);
    UNSERIALIZE_SCALAR(ISRV);
    bool apicTimerEventScheduled;
    UNSERIALIZE_SCALAR(apicTimerEventScheduled);
    if (apicTimerEventScheduled) {
        Tick apicTimerEventTick;
        UNSERIALIZE_SCALAR(apicTimerEventTick);
        if (apicTimerEvent.scheduled()) {
            reschedule(apicTimerEvent, apicTimerEventTick, true);
        } else {
            schedule(apicTimerEvent, apicTimerEventTick);
        }
    }
}

X86ISA::Interrupts *
X86LocalApicParams::create()
{
    //DPRINTF(LocalApic,"enter X86LocalApicParams::create()");
    return new X86ISA::Interrupts(this);
}
