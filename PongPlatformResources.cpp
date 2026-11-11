/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Shared Pong APPSS resources. Audio must not depend on the Wi-Fi driver
 * being selected. GPU retains its separate active TCS3 ownership. */
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include "PongPlatformResources.h"
#include "QcomScm.h"

extern "C" int armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *) __attribute__((weak_import));
extern "C" {volatile uint32_t native_ipcc_stats[4];} // IRQs,notifications,unowned,drain-limit

namespace {
IOLock *powerLock, *smmuLeaseLock, *scmCallLock,*ipccLock,*smemLocalLock,*qup5Lock;
IOMemoryMap *rscMap;
volatile uint8_t *rsc;
IOMemoryMap *ipccMap;
volatile uint8_t *ipcc;
bool ipccAttached;
struct IpccSubscriber {uint32_t id;void *context;PongIpccNotify notify;} ipccSubscribers[8];

uint32_t ipccRead(unsigned offset) {
    uint32_t value=*reinterpret_cast<volatile uint32_t *>(ipcc+offset);
    __asm__ volatile("dmb oshld" ::: "memory");return value;
}
void ipccWrite(unsigned offset,uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(ipcc+offset)=value;
    __asm__ volatile("dsb oshst" ::: "memory");
}
void ipccInterrupt(void *) {
    native_ipcc_stats[0]++;
    for(unsigned n=0;n<32;n++) {
        uint32_t id=ipccRead(QCOM_IPCC_RECV_ID);
        if(id==QCOM_IPCC_NONE)return;
        ipccWrite(QCOM_IPCC_RECV_CLEAR,id);
        bool handled=false;
        for(auto &entry:ipccSubscribers) {
            auto notify=__atomic_load_n(&entry.notify,__ATOMIC_ACQUIRE);
            if(notify && entry.id==id) {notify(entry.context);handled=true;native_ipcc_stats[1]++;break;}
        }
        if(!handled)native_ipcc_stats[2]++;
    }
    native_ipcc_stats[3]++; // bounded drain; hardware retains remaining IRQs
}

void initLock(IOLock **slot) {
    if (__atomic_load_n(slot, __ATOMIC_ACQUIRE)) return;
    IOLock *lock = IOLockAlloc(), *empty = nullptr;
    if (lock && !__atomic_compare_exchange_n(slot, &empty, lock, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) IOLockFree(lock);
}
uint64_t now() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}
uint32_t rd(unsigned off) {
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(rsc + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
void wr(unsigned off, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(rsc + off) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}
bool mapped() {
    if (rsc) return true;
    auto memory = IODeviceMemory::withRange(0x17a20000, 0x10000);
    if (!memory) return false;
    rscMap = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (rscMap) rsc = reinterpret_cast<volatile uint8_t *>(rscMap->getVirtualAddress());
    return rsc != nullptr;
}
bool fail(const char *message) {
    IOLog("Pong resources: %s\n", message);
    return false;
}
constexpr unsigned ActiveTcs=0xd00+4*0x2a0;
struct PendingVote {bool active=false,reported=false;uint32_t address=0,value=0;bool completed=false,progressed=false;};
PendingVote pendingVote;
bool controlWrite(uint32_t value) {
    wr(ActiveTcs+0x14,value);
    const uint64_t end=now()+10;
    do {
        if(rd(ActiveTcs+0x14)==value)return true;
        IODelay(10);
    }while(now()<end);
    return false;
}
bool retireVote(unsigned budgetMs) {
    if(!pendingVote.active)return true;
    const uint64_t end=now()+budgetMs;
    unsigned spins=0;
    do {
        const unsigned status=rd(ActiveTcs+0x3c),idle=rd(ActiveTcs+0x18);
        // Keep a real completion event/transition across retries, not just
        // old complete bits which might predate this programmed request.
        if((status&0x10100)!=0x10100||!idle)pendingVote.progressed=true;
        // CMD_STATUS_ISSUED/COMPL are Nothing's actual completion bits.
        // Match the owned request, idle engine and a completion event or
        // observed progress; never reclaim an unrelated/uncertain transfer.
        if((status&0x10100)==0x10100&&idle&&
           rd(ActiveTcs+0x1c)==1&&rd(ActiveTcs+0x34)==pendingVote.address&&
           rd(ActiveTcs+0x38)==pendingVote.value&&
           (pendingVote.progressed||(rd(0xd04)&(1u<<4))))pendingVote.completed=true;
        if(pendingVote.completed) {
            wr(ActiveTcs+0x1c,0);wr(0xd08,1u<<4);
            if(!controlWrite(rd(ActiveTcs+0x14)&~(1u<<24)))return false;
            if(pendingVote.reported)IOLog("Pong resources: retained RPMh request retired after real completion address=%x value=%x\n",pendingVote.address,pendingVote.value);
            pendingVote={};return true;
        }
        if(++spins%32)IODelay(10);else IOSleep(1);
    }while(now()<end);
    if(!pendingVote.reported) {
        pendingVote.reported=true;
        IOLog("Pong resources: RPMh request retained address=%x value=%x status=%x idle=%x control=%x irq=%x progressed=%u; no slot overwrite\n",
            pendingVote.address,pendingVote.value,rd(ActiveTcs+0x3c),rd(ActiveTcs+0x18),rd(ActiveTcs+0x14),rd(0xd04),pendingVote.progressed);
    }
    return false;
}
bool rawVote(uint32_t addr, uint32_t value) {
    constexpr unsigned b = 0xd00 + 4 * 0x2a0, other = 0xd00 + 3 * 0x2a0;
    if(pendingVote.active) {
        const bool same=pendingVote.address==addr&&pendingVote.value==value;
        if(!retireVote(100))return false;
        if(same)return true;
    }
    unsigned caps = rd(0xc), count = (caps >> 12) & 63, ncpt = (caps >> 27) & 31;
    if (((rd(0) >> 16) & 255) >= 3 || count < 8 || ncpt < 1 || !rd(b + 0x18))
        return fail("active TCS4 unavailable");
    if (!rd(other + 0x18)) {
        unsigned mask = rd(other + 0x1c);
        for (unsigned i = 0; i < ncpt && i < 16; i++)
            if ((mask & (1u << i)) && rd(other + 0x34 + 20 * i) == addr)
                return fail("conflicting RPMh transaction");
    }
    wr(0xd00, rd(0xd00) & ~(1u << 4));
    // Hardware requirement: clear trigger, read it back, then clear AMC
    // enable and read it back. Clearing both in one posted write violates
    // Nothing's __tcs_set_trigger ordering and can lose the next trigger.
    unsigned control=rd(b+0x14)&~(1u<<24);
    if(!controlWrite(control)||!controlWrite(control&~(1u<<16)))return fail("TCS4 trigger/enable clear not acknowledged");
    wr(b + 0x1c, 0); wr(0xd08, 1u << 4);
    uint64_t end = now() + 10;
    while (rd(0xd04) & (1u << 4)) {
        if (now() >= end) return fail("TCS4 completion did not clear");
        IOSleep(1);
    }
    wr(b + 0x30, 0x10108); wr(b + 0x34, addr); wr(b + 0x38, value);
    wr(b + 0x10, 1); wr(b + 0x1c, 1);
    if(rd(b+0x34)!=addr||rd(b+0x38)!=value||!controlWrite(1u<<16))return fail("TCS4 request/enable did not latch");
    pendingVote={true,false,addr,value};
    wr(b + 0x14, (1u << 16) | (1u << 24));
    return retireVote(100);
}
}

extern "C" void pong_rpmh_init(void) {
    initLock(&powerLock); initLock(&smmuLeaseLock);
}
extern "C" bool pong_rpmh_vote(uint32_t address, uint32_t value) {
    pong_rpmh_init();
    if (!powerLock) return false;
    IOLockLock(powerLock);
    bool ok = mapped() && rawVote(address, value);
    IOLockUnlock(powerLock);
    return ok;
}
extern "C" bool pong_qup5_lock(void) {
    initLock(&qup5Lock);if(!qup5Lock)return false;IOLockLock(qup5Lock);return true;
}
extern "C" void pong_qup5_unlock(void){if(qup5Lock)IOLockUnlock(qup5Lock);}
extern "C" bool pong_smmu_lease_lock(void) {
    pong_rpmh_init();
    if (!smmuLeaseLock) return false;
    IOLockLock(smmuLeaseLock);
    return true;
}
extern "C" void pong_smmu_lease_unlock(void) {
    if (smmuLeaseLock) IOLockUnlock(smmuLeaseLock);
}

/* A suspended/interrupted secure transaction owns its session across
 * resumes. Serialize the complete GPU/ADSP call, never individual SMCs. */
extern "C" bool pong_scm_call_lock(void) {
    initLock(&scmCallLock);if(!scmCallLock)return false;
    IOLockLock(scmCallLock);return true;
}
extern "C" void pong_scm_call_unlock(void) {
    if(scmCallLock)IOLockUnlock(scmCallLock);
}
extern "C" bool pong_smem_local_lock(void) {
    initLock(&smemLocalLock);if(!smemLocalLock)return false;
    IOLockLock(smemLocalLock);return true;
}
extern "C" void pong_smem_local_unlock(void) {
    if(smemLocalLock)IOLockUnlock(smemLocalLock);
}

/* Pong LPASS,AOP and MPSS share the same physical IPCC receiver/GIC261.
 * Never attach another handler which clears a sibling DSP's doorbell.
 */
extern "C" int pong_ipcc_subscribe(uint32_t client,uint32_t signal,PongIpccNotify notify,void *context) {
    if(client>0xffff || signal>0xffff || !notify)return kIOReturnBadArgument;
    initLock(&ipccLock);if(!ipccLock)return kIOReturnNoMemory;
    IOLockLock(ipccLock);int result=kIOReturnSuccess;
    if(!ipcc) {
        auto memory=IODeviceMemory::withRange(0x0ed18000,0x1000);
        ipccMap=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        if(memory)memory->release();
        if(ipccMap)ipcc=reinterpret_cast<volatile uint8_t *>(ipccMap->getVirtualAddress());
        if(!ipcc){IOLockUnlock(ipccLock);return kIOReturnNoMemory;}
    }
    if(!__atomic_load_n(&ipccAttached,__ATOMIC_ACQUIRE)) {
        result=armcpu_spi_attach?armcpu_spi_attach(261,false,ipccInterrupt,nullptr,nullptr,nullptr):kIOReturnNotReady;
        if(result){IOLockUnlock(ipccLock);return result;}
        __atomic_store_n(&ipccAttached,true,__ATOMIC_RELEASE);
        IOLog("Pong resources: shared IPCC receiver attached GIC261; per-owner doorbells\n");
    }
    const uint32_t id=qcom_ipcc_id(client,signal);IpccSubscriber *empty=nullptr;
    for(auto &entry:ipccSubscribers) {
        auto current=__atomic_load_n(&entry.notify,__ATOMIC_ACQUIRE);
        if(!current){if(!empty)empty=&entry;continue;}
        if(entry.id==id) {
            result=(current==notify && entry.context==context)?kIOReturnSuccess:kIOReturnExclusiveAccess;
            IOLockUnlock(ipccLock);return result;
        }
    }
    if(empty) {
        empty->id=id;empty->context=context;
        __atomic_store_n(&empty->notify,notify,__ATOMIC_RELEASE);
        ipccWrite(QCOM_IPCC_RECV_ENABLE,id);
    } else result=kIOReturnNoResources;
    IOLockUnlock(ipccLock);return result;
}
extern "C" bool pong_ipcc_send(uint32_t client,uint32_t signal) {
    if(client>0xffff || signal>0xffff || !__atomic_load_n(&ipccAttached,__ATOMIC_ACQUIRE))return false;
    ipccWrite(QCOM_IPCC_SEND,qcom_ipcc_id(client,signal));return true;
}
