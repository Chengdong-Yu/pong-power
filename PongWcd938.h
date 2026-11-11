/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pong SM8475 WCD938x prerequisites. Hardware inputs are the actual Pong
 * DTS and NothingOSS c402fcdb audio-kernel SWR/LPASS implementations.
 * AudioReach/PRM encoding is shared with UniXNU-sheng. Do not publish a
 * microphone until the physical capture graph actually returns PCM.
 */
struct PongWcd938 {
 Window va,tx,lpi,slew,swr;
 bool macroVote=false,codecVote=false,txClock=false,vaClock=false;
 unsigned stage=0,error=0,device=0,version=0,identity=0,transaction=0;
 uint64_t hardwareId=0;
 bool v17=false;
 unsigned fifoStatus=0x5050,readCommand=0x5024,readFifo=0x5040;
 bool prmBlock(unsigned block) {
  uint8_t bytes[512];const unsigned n=ar_prm_vote(bytes,sizeof bytes,AR_PRM_IID,++token,block,1);
  return command(bytes,n,AR_OP_PRM_REQUEST);
 }
 bool prmClock(unsigned id,unsigned rate) {
  uint8_t bytes[512];
  const unsigned rates[]={rate,rate==19200000?9600000u:19200000u},attributes[]={1,0};
  for(unsigned hz:rates)for(unsigned attribute:attributes) {
   const unsigned n=ar_prm_clock_attr(bytes,sizeof bytes,AR_PRM_IID,++token,id,hz,attribute);
   const bool accepted=command(bytes,n,AR_OP_PRM_REQUEST);
   IOLog("Pong WCD938: physical PRM clock=%x Hz=%u attribute=%u ACK=%u error=%d\n",id,hz,attribute,accepted,native_audio_stats[1]);
   if(accepted)return true;
  }
  return false;
 }
 bool rail() {
  Window window;if(!window.open(0x80860000,0x20000))return false;
  auto *db=static_cast<uint8_t *>(IOMallocData(0x20000));if(!db)return false;
  for(unsigned i=0;i<0x20000;i+=4){uint32_t value=window.r(i);memcpy(db+i,&value,4);}
  window.map->release();window.map=nullptr;window.v=nullptr;
  QcomRpmhResource resource{};const bool found=!qcom_cmd_db_find(db,0x20000,"smpb10",&resource);
  IOFreeData(db,0x20000);
  // Actual DT: PM8350 S10B supplies WCD RXTX/buck/IO at1.8V.
  return found&&pong_rpmh_vote(resource.address+8,7)&&pong_rpmh_vote(resource.address,1800)&&pong_rpmh_vote(resource.address+4,1);
 }
 bool fail(unsigned at,unsigned code) {
  stage=at;error=code;
  IOLog("Pong WCD938: prerequisite stage=%u error=%u master_version=%x; capture endpoint remains unpublished\n",stage,error,version);return false;
 }
 bool read(unsigned reg,uint8_t &result) {
  const unsigned id=(++transaction%14)+1;
  const auto fifo=swr.r(fifoStatus);
  if(((fifo>>8)&31)>=8)return false;
  swr.w(readCommand,(1u<<24)|(device<<20)|(id<<16)|reg);
  const uint64_t until=nowMs()+20;
  while(nowMs()<until) {
   if((swr.r(fifoStatus)>>16)&31) {
    const auto value=swr.r(readFifo);
    if(((value>>8)&15)==id){result=value&255;return true;}
    swr.w(0x308,1);return false;
   }
   IODelay(10);
  }
  return false;
 }
 bool open() {
  stage=1;if(!rail())return fail(1,ENODEV);
  // Reset and all analog configuration remain board-specific. No OTP,
  // calibration/MTP, ADC gain or microphone bias programming here.
  pin(43,(1u<<9)|(7u<<6),0);IOSleep(20);pin(43,(1u<<9)|(7u<<6),1);IOSleep(20);
  stage=2;if(!(macroVote=prmBlock(AR_HW_MACRO)))return fail(2,native_audio_stats[1]);
  if(!(codecVote=prmBlock(AR_HW_DCODEC)))return fail(3,native_audio_stats[1]);
  // SM8475 PRM must acknowledge both actual macro requests before any
  // LPASS macro/SoundWire access. A rejection skips dependent MMIO.
  // Actual Pong va_macro qcom,default-clk-id=0 selects TX_CORE_CLK,
  // as Nothing's lpass_cdc_va_macro_swr_clk uses TX_MCLK for this mode.
  // Do not require the independent VA-clock branch that the board does
  // not select. It returned a real DSP refusal in294. The shared physical
  // TX clock must ACK before touching either dependent macro or bus.
  if(!(txClock=prmClock(AR_CLK_TX_CORE_MCLK,19200000)))return fail(4,native_audio_stats[1]);
  vaClock=txClock;
  stage=6;
  if(!va.open(0x033f0000,0x1000)||!tx.open(0x03220000,0x1000)||!lpi.open(0x03440000,0x17000)||!slew.open(0x034da000,0x1000))return fail(6,ENOMEM);
  // Pong LPI pinctrl active state: TX clock0, data1/2/14, function1,
  // data bus-hold, slew1; original qcom,lpi-slew-offset-tbl.
  unsigned slewValue=slew.r(0);const unsigned pins[]={0,1,2,14},shifts[]={0,2,4,6};
  for(unsigned i=0;i<4;i++){unsigned old=lpi.r(pins[i]*0x1000);lpi.w(pins[i]*0x1000,(old&~0x3fu)|(1u<<2)|(i?2u:0u));slewValue=(slewValue&~(3u<<shifts[i]))|(1u<<shifts[i]);}
  slew.w(0,slewValue);
  // Original SM8475 fs-gen sequence and VA/TX clock gates. The VA
  // register offset0x3000 is a logical regmap index; this map is its DT PA.
  va.w(0,va.r(0)|1);va.w(4,va.r(4)|3);va.w(4,(va.r(4)&~2u)|1);va.w(0x80,va.r(0x80)|2);
  va.w(8,va.r(8)|2);va.w(8,va.r(8)|1);va.w(8,va.r(8)&~2u);
  tx.w(0,tx.r(0)|1);tx.w(4,tx.r(4)|1);
  if(!swr.open(0x033b0000,0x10000))return fail(7,ENOMEM);
  version=swr.r(0);v17=version==0x01070000u;
  if(!v17&&(version>>24)!=2)return fail(7,EPROTONOSUPPORT);
  if(v17){fifoStatus=0x328;readCommand=0x320;readFifo=0x334;}
  stage=8;
  swr.w(8,1);IOSleep(1);swr.w(0x101c,(1u<<3)|7u);swr.w(0x500,1);
  swr.w(0x1048,(swr.r(0x1048)&~(0x1fu<<17))|(0x1fu<<17));
  swr.w(0x18,1);
  if(v17) {
   // Nothing SM8475 CONFIG_SWRM_VER_1P7: CPU1 command FIFO/interrupt
   // offsets differ from the SM8550 v2 layout. CPU execution env1.
   swr.w(0x314,7);swr.w(0x1044,4);swr.w(4,2);
   swr.w(0x208,0xffffffffu);swr.w(0x210,0x1dfdfdu);swr.w(4,3);
  }else {
   swr.w(0x5060,1);swr.w(0x314,7u|(1u<<31));swr.w(4,1);swr.w(4,2);
   swr.w(0x5008,0xffffffffu);swr.w(0x5004,0x1ffffu);swr.w(4,3);
   if(!wait(swr,0x5064,1,1,500))return fail(8,ETIMEDOUT);
  }
  stage=9;unsigned status=0;
  for(unsigned i=0;i<200;i++) {
   status=swr.r(0x1090);
   for(unsigned n=1;n<12;n++)if((status>>(2*n))&3) {
    const uint64_t id=uint64_t(swr.r(0x530+8*n))|(uint64_t(swr.r(0x534+8*n))<<32);
    // Actual DTS reg=<0x0d 0x01170223>. Do not configure another slave.
    if((id&0xffffffffffffull)==0x000d01170223ull){device=n;hardwareId=id;break;}
   }
   if(device)break;IOSleep(5);
  }
  if(!device)return fail(9,ENODEV);
  uint8_t first=0,second=0;if(!read(0x50,first)||!read(0x51,second))return fail(10,EIO);
  identity=first|(unsigned(second)<<8);stage=11;
  IOLog("Pong WCD938: actual codec enumerated TXmaster=%x version=%x device=%u identity=%x; analog/capture route not yet published\n",0x033b0000,version,device,identity);
  return true;
 }
} pongWcd;
