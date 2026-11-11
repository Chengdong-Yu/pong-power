/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pong AW20036 boot illumination. Register facts and board current/channel
 * settings are from NothingOSS SM8475 c402fcdb3a1523d10b15ef1123bced45456f7592
 * (drivers/leds/leds-aw20036.*, gcc-waipio, actual Pong DT).
 * Dedicated QUP0 SE5 FIFO owner; never use the TFA SE13/audio bus.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "GeniI2c.h"
#include "PongPlatformResources.h"
namespace {
struct Window {
    IOMemoryMap *map=nullptr;volatile uint8_t *base=nullptr;
    bool open(uint64_t pa,size_t bytes) {
        auto *m=IODeviceMemory::withRange(pa,bytes);map=m?m->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        if(m)m->release();base=map?reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress()):nullptr;return base;
    }
    unsigned read(unsigned off){unsigned n=*reinterpret_cast<volatile uint32_t *>(base+off);__asm__ volatile("dmb oshld":::"memory");return n;}
    void write(unsigned off,unsigned n){*reinterpret_cast<volatile uint32_t *>(base+off)=n;__asm__ volatile("dsb oshst":::"memory");}
    void close(){if(map)map->release();map=nullptr;base=nullptr;}
} gcc,tlmm,engine;
GeniI2c bus;
void pad(unsigned n,unsigned config,int output) {
    const unsigned at=n*0x1000,old=tlmm.read(at);
    config=(old&~0x3ffu)|(config&0x3ffu);if(old&(1u<<11))config|=1u<<12;
    if(output>=0)tlmm.write(at+4,output?2:0);
    tlmm.write(at,config);if(output>=0)tlmm.write(at+4,output?2:0);
}
bool transfer(unsigned reg,unsigned value,bool reading=false) {
    int result=reading?geni_i2c_start_read(&bus,uint8_t(reg)):geni_i2c_start_write(&bus,uint8_t(reg),uint8_t(value));
    if(result)return false;
    for(unsigned phase=0;phase<3;phase++) {
        for(unsigned n=0;n<500;n++) {
            result=geni_i2c_poll(&bus);if(result==GeniDone)return true;if(result==GeniFailed)return false;IODelay(20);
        }
        if(geni_i2c_timeout(&bus)!=GeniPending)break;
    }
    return false;
}
bool configureBus() {
    if(!gcc.open(0x100000,0xa0000)||!tlmm.open(0xf100000,0xd0000)||!engine.open(0x994000,0x4000))return false;
    // Existing APPS GCC vote bits: wrapper0 M/S AHB and SE5, original DT.
    gcc.write(0x62008,gcc.read(0x62008)|(1u<<6)|(1u<<7)|(1u<<15));
    static const unsigned clocks[]={0x27004u,0x27008u,0x27610u};
    for(unsigned at:clocks) {
        for(unsigned n=0;n<100&&(gcc.read(at)&0x80000000u)&&!(gcc.read(at)&2);n++)IOSleep(1);
        if((gcc.read(at)&0x80000000u)&&!(gcc.read(at)&2))return false;
    }
    if(((engine.read(GENI_FW_REVISION)>>8)&255)!=GENI_PROTO_I2C)return false;
    // Select the actual boot-programmed19.2MHz TCXO DFS entry. Do not
    // program an unreviewed RCG/PLL rate if that entry is unavailable.
    unsigned level=8;
    for(unsigned i=0;i<8;i++) {
        unsigned cfg=gcc.read(0x27618+0x1c+4*i);
        if(((cfg>>8)&7)==0&&((cfg>>12)&3)==0&&(cfg&31)<=1){level=i;break;}
    }
    if(level==8)return false;
    pad(206,3u|(1u<<2),-1);pad(207,3u|(1u<<2),-1);
    pad(60,3u|(3u<<6),-1); // actual active-low IRQ input; no IRQ handler needed for fixed output
    pad(44,(3u<<6)|(1u<<9),0);IOSleep(1);pad(44,(3u<<6)|(1u<<9),1);IOSleep(3);
    engine.write(0x28,engine.read(0x28)|0x7f);engine.write(0xe30,engine.read(0xe30)|15);
    engine.write(0x24,0x7f);engine.write(0x20,1);engine.write(0xe18,0);engine.write(0x7c,level);
    engine.write(GENI_CLK_CFG,0x21);engine.write(GENI_SCL_COUNTERS,(5u<<20)|(11u<<10)|22);
    GeniIo io={[](void *,uint32_t off){return engine.read(off);},[](void *,uint32_t off,uint32_t n){engine.write(off,n);},nullptr};
    unsigned config=0;return !geni_i2c_prepare(&bus,&io,0x3a,&config);
}
void worker(void *,wait_result_t) {
    thread_set_thread_name(current_thread(),"pong_glyph");
    // Run after normal boot has established shared clocks and memory.
    IOSleep(15000);
    if(!pong_qup5_lock()){IOLog("PongGlyph: shared QUP5 ownership unavailable; normal boot retained\n");return;}
    bool ok=configureBus();unsigned chip=0;
    if(ok){ok=transfer(0xf0,0xc0)&&transfer(0,0,true);chip=bus.data;ok=ok&&chip==0x18;}
    if(ok) {
        ok=transfer(2,1);IOSleep(3);
        // Nothing production current code5, ALLON, three rows; retain
        // per-channel current balancing. Off channels1/25 stay off: these
        // are not missing Glyph segments and must not be overdriven.
        static const uint8_t dim[36]={
            38,0,4,4,4,4,4,4,4,4,3,4,
            53,63,4,4,4,4,4,4,4,2,4,4,
            33,0,4,5,30,27,63,30,21,2,4,4};
        static const uint8_t config[][2]={{3,0x58},{0x80,2},{0x81,0x11},{0x82,0x36},{0x83,0xff},{0x84,4},{0x85,0x58},{0x86,3}};
        for(auto &r:config)if(ok)ok=transfer(r[0],r[1]);
        // SW reset leaves SLPCR.SLEEP set: pages1..5 ignore writes while
        // sleeping. Wake first, wait >=200us, then use the vendor driver's
        // page4 paired DIM/FADE layout; waking after SRAM writes loses them.
        if(ok)ok=transfer(1,0);IOSleep(1);
        if(ok)ok=transfer(1,0,true)&&!(bus.data&0x80);
        if(ok)ok=transfer(0xf0,0xc4);
        for(unsigned n=0;ok&&n<36;n++)ok=transfer(n*2,dim[n])&&transfer(n*2+1,dim[n]?0x80:0);
        if(ok)ok=transfer(0xf0,0xc0);
        unsigned mode=~0u,current=~0u,size=~0u;
        if(ok&&transfer(1,0,true))mode=bus.data;else ok=false;
        if(ok&&transfer(3,0,true))current=bus.data;else ok=false;
        if(ok&&transfer(0x80,0,true))size=bus.data;else ok=false;
        IOLog("PongGlyph: actual control registers sleep=%x global=%x rows=%x; paired page4 programmed after wake\n",mode,current,size);
    }
    if(!ok&&chip==0x18){(void)transfer(0xf0,0xc0);(void)transfer(1,0x80);}
    IOLog("PongGlyph: AW20036 boot all-segments %s chip=%x bus=994000 address=3a current=stock5 fade=128 error=%u irq=%x; normal boot retained\n",
        ok?"PASS":"SKIP",chip,bus.error,bus.error_status);
    engine.close();tlmm.close();gcc.close();
    pong_qup5_unlock();
}
}
extern "C" void pong_glyph_start_optional(void) {
    unsigned enabled=1;PE_parse_boot_argn("pong_glyph",&enabled,sizeof enabled);if(!enabled)return;
    static uint32_t started;if(__atomic_exchange_n(&started,1,__ATOMIC_ACQ_REL))return;
    thread_t thread=nullptr;
    if(kernel_thread_start(worker,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
    else IOLog("PongGlyph: worker allocation failed; normal boot retained\n");
}
