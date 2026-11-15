// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Layer812
// VinylM5 Dial Edition R1F27 (guarded Core0 LCD, priority Core1 input) - M5Dial Port.B + Unit AudioPlayer U197
// 2026-11-15 | No external assets | N9301 command interval >= 520 ms
#include <Arduino.h>
#include <M5Dial.h>
#include <math.h>
#include <esp_system.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>

// Optional USB logging must never stall control when the host is absent/slow.
// Format into bounded stack storage; emit ONLY when the entire string fits
// the CDC TX queue (so Serial.write cannot wait for the host).
static void vinylLogf(const char* fmt, ...) {
  if (!Serial) return;
  char line[160];
  va_list args; va_start(args,fmt);
  const int n=vsnprintf(line,sizeof(line),fmt,args);
  va_end(args);
  if(n>0 && n<int(sizeof(line)) && Serial.availableForWrite()>=n)
    Serial.write(reinterpret_cast<const uint8_t*>(line),size_t(n));
}
static void vinylLogln(const char* line="") {
  vinylLogf("%s\n",line);
}
#define VINYL_LOGF(...) vinylLogf(__VA_ARGS__)
#define VINYL_LOGLN(...) vinylLogln(__VA_ARGS__)

// Physical-device access is serialized across cores, with a short timeout.
// Never acquire this mutex in the image computation or while holding a spinlock.
static SemaphoreHandle_t m5HardwareMutex = nullptr;
#include "label_assets.h"

static HardwareSerial playerUart(1);
static constexpr int RX_PIN = 1;   // Dial PORT.B white <- AudioPlayer TX
static constexpr int TX_PIN = 2;   // Dial PORT.B yellow -> AudioPlayer RX
static constexpr uint32_t CMD_GAP_MS = 540;
static constexpr uint32_t STARTUP_MS = 5200;
// Give the N9301 one additional second AFTER it reports SD presence.
// This is deliberately not "one second after the arbitrary 5.2s timer".
static constexpr uint32_t SD_SETTLE_MS = 1000;
static constexpr uint32_t SD_REPROBE_MS = 2700;

// Packet: CMD, ~CMD, LEN, DATA..., low8(sum(all previous bytes)).
struct N9301 {
  uint32_t lastTx=0, bootAt=0, lastPlayTx=0, nameQueryAt=0, nameDemandAt=0, lastNameRetryAt=0;
  uint8_t nameRetries=0;
  bool ready=false, present=false, statusReceived=false, statusKnown=false;
  bool deviceReplyKnown=false;
  uint32_t sdDetectedAt=0, lastSdProbeAt=0;
  uint8_t volume=10, status=0; // 0 stopped, 1 playing, 2 paused
  uint32_t statusAt=0;
  uint16_t track=1, total=0;
  bool loopModeKnown=false; uint8_t loopMode=255;
  bool trackKnown=false;
  bool trackChanged=false;
  bool fileError=false;
  uint8_t lastError=0;
  uint32_t trackReplySequence=0;
  uint32_t elapsed=0, duration=0, timeStamp=0;
  bool validElapsed=false;
  char shortName[16]="";
  bool shortNameKnown=false;
  uint32_t nameReplySequence=0;
  struct Job { uint8_t cmd, n, data[6]; } queue[16];
  uint8_t head=0, tail=0;
  uint8_t rx[40]; uint8_t rlen=0, need=0;
  uint32_t rxAt=0;
  uint32_t crcErrors=0, rxFrames=0, txFrames=0;
  static void logPacket(const char* dir,uint8_t cmd,const uint8_t* data,uint8_t n){
    VINYL_LOGF("[%10lu] %s CMD=%02X LEN=%u DATA=",(unsigned long)millis(),dir,cmd,(unsigned)n);
    for(uint8_t i=0;i<n;i++)VINYL_LOGF("%02X",data[i]);
    VINYL_LOGLN();
  }
  void begin(){
    playerUart.begin(9600,SERIAL_8N1,RX_PIN,TX_PIN);
    bootAt=millis();
  }
  void enqueue(uint8_t cmd, uint8_t a) { uint8_t d[]={a}; enqueueData(cmd,d,1); }
  void enqueueData(uint8_t cmd,const uint8_t *d,uint8_t n){
    if(n>6) return;
    uint8_t next=(uint8_t)((tail+1)%16);
    if(next==head) return;
    Job &j=queue[tail]; j.cmd=cmd;j.n=n;
    for(uint8_t i=0;i<n;i++) j.data[i]=d[i];
    tail=next;
  }
  void clearPending(){head=tail;}
  void packet(const Job& j){
    uint8_t sum=(uint8_t)(j.cmd + (uint8_t)(~j.cmd) + j.n);
    playerUart.write(j.cmd);playerUart.write((uint8_t)~j.cmd);playerUart.write(j.n);
    for(uint8_t i=0;i<j.n;i++){playerUart.write(j.data[i]);sum+=j.data[i];}
    playerUart.write(sum);
    playerUart.flush();
    lastTx=millis();
    if(j.cmd==0x04 && ((j.n==1 && j.data[0]==0x01) ||
                       (j.n==3 && j.data[0]==0x06))) {
      // N9301 0x06 SELECT TRACK is itself a PLAY command (per official protocol).
      lastPlayTx=lastTx;
      statusKnown=false;
    }
    if(j.cmd==0x04 && j.n==1 && j.data[0]==0x15) nameQueryAt=lastTx;
    ++txFrames;
    logPacket("TX",j.cmd,j.data,j.n);
  }
  bool canSend(uint32_t now) const{return ready && ((uint32_t)(now-lastTx)>=CMD_GAP_MS);}
  void update(uint32_t now){
    if(!ready && now-bootAt>=STARTUP_MS){
      ready=true;lastTx=now-CMD_GAP_MS;
      enqueue(0x04,0x08); // online devices (SD presence is not assumed)
      lastSdProbeAt=now;
    }
    while(playerUart.available()) parse((uint8_t)playerUart.read(),now);
    // Never enqueue repeated probes once the module has acknowledged the SD.
    if(ready && !sdDetectedAt && (uint32_t)(now-lastSdProbeAt)>=SD_REPROBE_MS){
      enqueue(0x04,0x08);
      lastSdProbeAt=now;
    }
    if(canSend(now)&&head!=tail){Job j=queue[head];head=(head+1)%16;packet(j);}
  }
  void parse(uint8_t b,uint32_t now){
    if(rlen && now-rxAt>500){rlen=0;need=0;}
    rxAt=now;
    if(rlen==0){if(b!=0x04 && b!=0x05 && b!=0x06 && b!=0x0A && b!=0x0B && b!=0xAA && b!=0xBB) return;rx[rlen++]=b;return;}
    if(rlen==1){if(b!=(uint8_t)(~rx[0])){rlen=0;return;}rx[rlen++]=b;return;}
    if(rlen==2){if(b>28){rlen=0;return;}rx[rlen++]=b;need=(uint8_t)(b+4);if(need>sizeof(rx))rlen=0;return;}
    rx[rlen++]=b;
    // The official PDF prints the 0x0D total-tracks reply as LEN=02 while
    // showing sub-command + two count bytes (3 data bytes). Accept this
    // inconsistent wire variant, as well as the normal LEN=03 format.
    const bool totalLen02=(rx[0]==0x04 && rx[2]==0x02 &&
                           rlen>=4 && rx[3]==0x0D);
    if(totalLen02)need=7;
    if(rlen<need)return;
    uint8_t check=0;for(uint8_t i=0;i<need-1;i++)check+=rx[i];
    if(check==rx[need-1]){
      ++rxFrames;
      if(totalLen02){
        total=(uint16_t(rx[4])<<8)|rx[5];
        VINYL_LOGF("[%10lu] N9301 TOTAL=%u (LEN=02 variant)\n",
           (unsigned long)now,unsigned(total));
      }else{
        logPacket("RX",rx[0],&rx[3],rx[2]);
        decode();
      }
    }else{
      ++crcErrors;
      VINYL_LOGF("[%10lu] N9301 BAD_CHECKSUM count=%lu\n",(unsigned long)now,(unsigned long)crcErrors);
    }
    rlen=0;need=0;
  }
  void decode(){
    uint8_t cmd=rx[0], n=rx[2], *d=&rx[3];
    if(cmd==0x04 && n>=2){
      switch(d[0]){
        case 0x00: if(d[1]<=2){
          VINYL_LOGF("[%10lu] N9301 STATUS=%u previous=%u\n",(unsigned long)millis(),(unsigned)d[1],(unsigned)status);
          status=d[1];statusAt=millis();present=true;statusReceived=true;statusKnown=true;
        }break;
        case 0x08:
          deviceReplyKnown=true;
          present=(d[1]&0x02)!=0;
          if(present && !sdDetectedAt){
            sdDetectedAt=millis();
            VINYL_LOGF("[%10lu] SD PRESENT via 0x08; settle %lu ms before configuration/play\n",
                        (unsigned long)sdDetectedAt,(unsigned long)SD_SETTLE_MS);
          } else if(!present){
            sdDetectedAt=0;
            VINYL_LOGF("[%10lu] SD NOT DETECTED 0x08=%02X\n",
                        (unsigned long)millis(),unsigned(d[1]));
          }
          break;
        case 0x0D: if(n>=3){
          total=(uint16_t(d[1])<<8)|d[2];
          VINYL_LOGF("[%10lu] N9301 TOTAL=%u\n",(unsigned long)millis(),unsigned(total));
        }break;
        case 0x0E: if(n>=3){
          uint16_t newTrack=uint16_t((uint16_t(d[1])<<8)|d[2]);
          if(!trackKnown || newTrack!=track){
            trackChanged=true;
            shortNameKnown=false;
            shortName[0]=0;
            fileError=false;
          }
          track=newTrack;
          trackKnown=true;
          trackReplySequence++;
        }break;
        case 0x15:
          if(n>=12){
            for(uint8_t i=0;i<11;i++){
              const uint8_t c=d[i+1];
              shortName[i]=(c>='a' && c<='z')?char(c-'a'+'A'):
                             ((c>=32 && c<=126)?char(c):'_');
            }
            shortName[11]=0;
            shortNameKnown=true; nameReplySequence++; nameDemandAt=0;
          }
          break;
        default:break;
      }
    }
    // N9301 errors use a separate valid frame type: AA 55 02 FF ERR ERR.
    // This was silently ignored in all prior releases, hiding file-read failures.
    if(cmd==0xAA && n==2 && d[0]==0xFF){
      lastError=d[1];
      const char* reason="UNKNOWN";
      if(lastError==1)reason="UART FRAME ERROR";
      else if(lastError==2)reason="DEVICE NOT FOUND";
      else if(lastError==3)reason="NO PLAYABLE DEVICE";
      else if(lastError==4){reason="FILE PLAYBACK ERROR";fileError=true;shortNameKnown=false;}
      VINYL_LOGF("[%10lu] N9301 ERROR=%u %s\n", (unsigned long)millis(),
                    (unsigned)lastError, reason);
    }
    if(cmd==0xBB && n==1){
      VINYL_LOGF("[%10lu] N9301 MEDIA EVENT=%02X (insert/remove indistinguishable)\n",
                    (unsigned long)millis(),(unsigned)d[0]);
    }
    if(cmd==0x0B && n>=2 && d[0]==0x00){
      loopMode=d[1]; loopModeKnown=true;
      VINYL_LOGF("[%10lu] LOOP MODE READBACK=%u %s\n",(unsigned long)millis(),
          unsigned(loopMode),loopMode==0?"ALL_LOOP":"UNEXPECTED");
    }
    if(cmd==0x0A && n>=4 && d[0]==0x01){
      elapsed=(uint32_t)d[1]*3600+(uint32_t)d[2]*60+d[3];
      timeStamp=millis();validElapsed=true;
    }
    if(cmd==0x06 && n>=2 && d[0]==0x00){volume=d[1];}
  }
  void control(uint8_t c){enqueue(0x04,c);}
  void selectTrack(uint16_t t){
    uint8_t d[]={0x06,uint8_t(t>>8),uint8_t(t)};
    // Replace stale track-selection jobs, preserving other commands in order.
    for(uint8_t i=head;i!=tail;i=(uint8_t)((i+1)%16)){
      Job &q=queue[i];
      if(q.cmd==0x04 && q.n==3 && q.data[0]==0x06){
        q.data[1]=d[1];q.data[2]=d[2];return;
      }
    }
    enqueueData(0x04,d,3);
  }
  void setVolume(uint8_t v){
    volume=constrain(v,0,30);
    for(uint8_t i=head;i!=tail;i=(uint8_t)((i+1)%16)){
      Job &q=queue[i];
      if(q.cmd==0x06 && q.n==2 && q.data[0]==0x01){q.data[1]=volume;return;}
    }
    uint8_t d[]={0x01,volume};enqueueData(0x06,d,2);
  }
  void seekTo(uint32_t seconds){
    // Protocol 0x0F has minute and second fields; maximum representable 255:59.
    if(seconds>15359)seconds=15359;
    uint8_t d[]={0x0F,(uint8_t)(seconds/60),(uint8_t)(seconds%60)};
    enqueueData(0x04,d,3);
    elapsed=seconds;timeStamp=millis();validElapsed=true;
  }
} audio;

// Core1 writes playback and input state; Core0 only reads a locked snapshot.
// LCD and input share one short-lived hardware mutex, UART belongs to Core1.
struct DisplayState {
  bool playing = false;
  bool nameUnavailable = false;
  bool uartSilent = false;
  bool noSD = false;
  bool ready = false;
  bool nameKnown = false;
  bool fileError = false;
  uint16_t track = 1;
  uint16_t total = 0;
  bool trackKnown = false;
  char name[16] = {};
  char toast[24] = {};
  uint32_t popupUntil = 0;
  uint32_t turnAt = 0;
  float turnSpeed = 0;
};
static portMUX_TYPE displayMux = portMUX_INITIALIZER_UNLOCKED;
static DisplayState displayShared;
static TaskHandle_t displayTaskHandle = nullptr;
static bool playing=false, playRequested=false, userPaused=false, touched=false;
static uint32_t inputLoopCount=0, inputMaxUpdateMs=0;
static Preferences userPrefs;
static uint16_t restoreTrack=0;
static uint32_t lastVolumeChanged=0;
static uint32_t lastInputAt=0;
static uint8_t lastSavedVolume=255;
static uint16_t lastSavedTrack=0;
static uint32_t touchAt=0, lastTouchTapAt=0, lastTurn=0, popupUntil=0;
static uint32_t lastTrackSend=0, trackRefreshDue=0;
// M5Dial: 16 detents / 64 quadrature counts per physical turn.
// A slow FULL TURN means one song. Rapid multiple turns are coalesced into
// one global-index command instead of a series of NEXT commands.
// The actual m5stack/M5Dial Encoder.h advances the *position* on each
// quadrature transition. M5Dial is 16 detents / 64 counts per physical turn.
// R1F26 did not send a track command when the current 0x0E track index was
// unknown; that was the cause of "the knob stopped working", not the scale.
static constexpr int ENCODER_COUNTS_PER_REV = 64;
static int pendingEncoderCounts=0;
static int sx=0,sy=0,px=0,py=0;
static long oldEnc=0;
static float angularSpeed=0;
static char toast[24]="";

static void popup(const char* s,uint32_t duration=850){snprintf(toast,sizeof(toast),"%s",s);popupUntil=millis()+duration;}
static void requestTrack(int direction){
  VINYL_LOGF("[%10lu] USER track direction=%d\n",(unsigned long)millis(),direction);
  audio.control(direction>0?0x05:0x04);
  userPaused=false; playRequested=true; // track navigation is a playback request
  audio.validElapsed=false;
  audio.shortNameKnown=false;
  audio.shortName[0]=0;
  audio.nameDemandAt=millis();
  audio.nameRetries=0;
  audio.lastNameRetryAt=0;
  audio.fileError=false;
  lastInputAt=millis();
  trackRefreshDue=millis()+950;
  popup(direction>0?"NEXT TRACK":"PREV TRACK");
}
// Navigation results are checked against the N9301 0x0E track response.
// An on-screen label is NOT evidence that the module changed folders.
static bool folderAwait=false;
static bool folderPollQueued=false;
static bool folderHadBaseline=false;
static uint16_t folderOldTrack=0;
static uint32_t folderRequestAt=0, folderOldReplySeq=0;
static int folderDirection=0;
static bool folderPlayQueued=false;
static void folderMove(int direction){
  VINYL_LOGF("[%10lu] USER folder direction=%d\n",(unsigned long)millis(),direction);
  if(folderAwait || !audio.ready) return;
  folderDirection=direction;
  folderHadBaseline=audio.trackKnown;
  folderOldTrack=audio.track;
  folderOldReplySeq=audio.trackReplySequence;
  folderRequestAt=millis();
  folderPollQueued=false;
  folderPlayQueued=false;
  folderAwait=true;
  audio.control(direction>0?0x13:0x12);
  userPaused=false; playRequested=true;
  audio.validElapsed=false;
  audio.shortNameKnown=false;
  audio.shortName[0]=0;
  audio.nameDemandAt=millis();
  audio.nameRetries=0;
  audio.lastNameRetryAt=0;
  audio.fileError=false;
  lastInputAt=millis();
  trackRefreshDue=millis()+1200;
  popup(direction>0?"NEXT FOLDER...":"PREV FOLDER...",1500);
}
static void folderNavigationUpdate(uint32_t now){
  if(!folderAwait)return;
  // N9301 0x12/0x13 are supported. The protocol does not report the folder NAME;
  // track-number changes are NOT proof of moving from A to B or B to C.
  if(!folderPollQueued && now-folderRequestAt>=1050){
    audio.enqueue(0x04,0x00); // query status first
    audio.enqueue(0x04,0x0E); // then current global track
    folderPollQueued=true;
  }
  if(!folderPlayQueued && now-folderRequestAt>=2400){
    // Some N9301 folder transitions stop playback. Restart only if confirmed stopped;
    // do not reset a successfully playing track with a redundant PLAY.
    if(audio.statusKnown && audio.statusAt>=folderRequestAt){
      if(audio.status==0){audio.control(0x01); playRequested=true;}
      folderPlayQueued=true;
    }
  }
  if(now-folderRequestAt>=4400){
    folderAwait=false;
    const bool changed=folderHadBaseline && audio.trackKnown && audio.track!=folderOldTrack;
    popup(changed?"TRACK CHANGED":"FOLDER ?",1200);
    VINYL_LOGF("[%10lu] FOLDER COMMAND direction=%d trackChanged=%d old=%u new=%u; folder name unavailable by UART\n",
         (unsigned long)now,folderDirection,int(changed),unsigned(folderOldTrack),unsigned(audio.track));
    trackRefreshDue=now+200;
  }
}
static void togglePlay(){
  VINYL_LOGF("[%10lu] USER encoder button: toggle play/stop\n",(unsigned long)millis());
  if(userPaused){
    userPaused=false;
    playRequested=true;
  }else{
    playRequested=!playRequested;
  }
  audio.control(playRequested?0x01:0x03);
  if(!playRequested) playing=false;
  lastInputAt=millis();
  popup(playRequested?"PLAY":"STOP");
}
// Central tap is PAUSE/RESUME (N9301 0x02 / 0x01), not STOP.
// The physical encoder button retains PLAY/STOP as an independent control.
static void togglePause(){
  const uint32_t now=millis();
  if(userPaused || !playRequested){
    userPaused=false;
    playRequested=true;
    audio.control(0x01);
    popup("RESUME");
    VINYL_LOGF("[%10lu] USER center tap RESUME\n",(unsigned long)now);
  }else{
    userPaused=true;
    playRequested=false; // also disarm unexpected-STOP auto-recovery
    playing=false;
    audio.control(0x02);
    popup("PAUSE");
    VINYL_LOGF("[%10lu] USER center tap PAUSE\n",(unsigned long)now);
  }
  lastInputAt=now;
}
static void tapAction(int x,int y){
  // Center PAUSE, four radial sectors: top/bottom volume, right/left folder.
  // Ignore duplicate M5Unified wasClicked() after the same touch release.
  const uint32_t now=millis();
  if(lastTouchTapAt && (uint32_t)(now-lastTouchTapAt)<90) return;
  lastTouchTapAt=now;
  const int dx=x-120,dy=y-120;
  if(dx*dx+dy*dy<=30*30){togglePause();return;}
  if(abs(dy)>abs(dx)){
    if(dy<0)audio.setVolume(min(30,int(audio.volume)+1));
    else audio.setVolume(max(0,int(audio.volume)-1));
    lastVolumeChanged=now;lastInputAt=now;
    char msg[20];snprintf(msg,sizeof(msg),"VOL %02u",audio.volume);
    popup(msg,900);
  }else{
    folderMove(dx>0?1:-1);
  }
}
// Single touch release only. Center PAUSE/RESUME; encoder push is PLAY/STOP.
// Spurious M5Unified click copies are deduplicated in tapAction().
static void touchUpdate(uint32_t now){
  auto t=M5Dial.Touch.getDetail();
  bool down=t.isPressed();
  if(down && !touched){touched=true;sx=px=t.x;sy=py=t.y;touchAt=now; VINYL_LOGF("[%lu] TOUCH DOWN %d,%d\n",(unsigned long)now,sx,sy);}
  else if(down){px=t.x;py=t.y;}
  else if(!down && touched){
    touched=false;
    VINYL_LOGF("[%lu] TOUCH UP %d,%d\n",(unsigned long)now,px,py);
    int dx=px-sx,dy=py-sy;
    uint32_t held=now-touchAt;
    if(held<35 || held>700) return; // ignore glitches and held touches
    if(abs(dx)>45 && abs(dx)>abs(dy)){requestTrack(dx<0?1:-1);}
    else if(abs(dx)<25 && abs(dy)<25){tapAction(sx,sy);}
  } else if(!down && !touched && t.wasClicked()) {
    // M5Unified can report an entire short tap as a click event between polls.
    // Recover it instead of discarding a real user action.
    VINYL_LOGF("[%lu] TOUCH CLICK %d,%d\n",(unsigned long)now,t.x,t.y);
    tapAction(t.x,t.y);
  }
}
// Send at most one track command per UART command interval, so a quick turn
// never fills the N9301 queue with metadata requests.
static void encoderUpdate(uint32_t now){
  long pos=M5Dial.Encoder.read();
  long delta=pos-oldEnc;
  if(delta==0)return;
  oldEnc=pos;
  VINYL_LOGF("[%lu] ENCODER delta=%ld\n",(unsigned long)now,delta);
  // Accumulate 64 raw quadrature counts/turn, including partial rotations.
  // Opposite turns cancel; rapid spins are coalesced into one 0x06 selection.
  pendingEncoderCounts=constrain(pendingEncoderCounts+
      (int)constrain(delta,-2048L,2048L),-8192,8192);
  lastTurn=now;
  angularSpeed=constrain((float)delta*0.11f,-1.3f,1.3f);
  lastInputAt=now;
}
static void trackDialUpdate(uint32_t now){
  // User input is buffered while SD is loading, never issued prematurely.
  if(!audio.ready || !audio.sdDetectedAt ||
      (uint32_t)(now-audio.sdDetectedAt)<SD_SETTLE_MS ||
      abs(pendingEncoderCounts)<ENCODER_COUNTS_PER_REV)return;
  // Wait for the end of a quick spin so 2+ complete revolutions become a
  // SINGLE global-index selection (never send NEXT 1, NEXT 2, ...).
  if(now-lastTurn<220 || (lastTrackSend && now-lastTrackSend<610))return;
  const int turns=pendingEncoderCounts / ENCODER_COUNTS_PER_REV;
  if(turns==0)return;
  if(audio.trackKnown && audio.track>=1){
    const int current=int(audio.track);
    const int total=int(audio.total);
    int target=0;
    if(total>0){
      int index=((current-1+turns)%total+total)%total;
      target=index+1;
    }else{
      target=constrain(current+turns,1,65535);
    }
    // Consume only complete turns. Preserve fractional motion for the next
    // action, including reverse direction, independent of UART timing.
    pendingEncoderCounts-=turns*ENCODER_COUNTS_PER_REV;
    const uint16_t destination=uint16_t(target);
    VINYL_LOGF("[%10lu] DIAL turns=%d current=%u total=%u target=%u remainder=%d\n",
      (unsigned long)now,turns,unsigned(audio.track),unsigned(audio.total),
      unsigned(destination),pendingEncoderCounts);
    if(destination!=audio.track){
      audio.selectTrack(destination);
      userPaused=false; playRequested=true;
      audio.track=destination;
      audio.shortNameKnown=false;
      audio.shortName[0]=0;
      audio.nameDemandAt=now;
      audio.nameRetries=0;
      audio.lastNameRetryAt=0;
      audio.fileError=false;
      audio.validElapsed=false;
      trackRefreshDue=now+1100;
      popup("TRACK SELECT",750);
    }
  }else{
    // NO NAME and missing track-index replies must not disable the knob.
    // No valid absolute index: keep the knob functional with a guarded NEXT
    // or PREV command. Request 0x0E again; if the decoder returns an index,
    // the remaining turns become a single 0x06 jump instead of N commands.
    static uint32_t lastRelativeAt=0;
    if(!lastRelativeAt || now-lastRelativeAt>=CMD_GAP_MS){
      const int dir=turns>0?1:-1;
      requestTrack(dir); // 0x05 NEXT / 0x04 PREV, does not need known index
      pendingEncoderCounts-=dir*ENCODER_COUNTS_PER_REV;
      lastRelativeAt=now;
      audio.enqueue(0x04,0x0E); // refresh absolute index for subsequent turns
      lastTrackSend=now;
      VINYL_LOGF("[%lu] DIAL relative fallback=%d, remaining=%d counts\n",
        (unsigned long)now,dir,pendingEncoderCounts);
    }
    return;
  }
  lastTrackSend=now;
}
static void trackMetadataUpdate(uint32_t now){
  if(!trackRefreshDue || now<trackRefreshDue ||
     abs(pendingEncoderCounts)>=ENCODER_COUNTS_PER_REV)return;
  trackRefreshDue=0;
  audio.enqueue(0x04,0x0E); // current track number
  audio.enqueue(0x04,0x15); // 8.3 short name of current track
}
// R1F27: Core0 composes raw RGB565 pixels without hardware calls.
// Core1 handles LCD, touch, knob, buttons, UART and NVS.
// The R1F13/R1F14 crash happened on Core 0 at address 0x74, immediately
// after the display task started. Both had a second LGFX_Sprite and an
// off-screen pushSprite path. Eliminate that entire path, including getBuffer.
// Fixed storage avoids allocation failures or sprite parent-pointer issues.
static constexpr int LABEL_W=122, FRAME_W=224;
static_assert(LABEL_W<=FRAME_W, "label must fit inside frame");
static uint16_t labelPixels[LABEL_W*LABEL_W];
// One complete 224x224 vinyl frame, 100352 bytes, versus 2x136x136 patches.
// The producer and consumer strictly alternate ownership; no concurrent writes.
static uint16_t framePixels[FRAME_W * FRAME_W];
static uint8_t radialCache[(FRAME_W/2+1)*(FRAME_W/2+1)]; // quadrant symmetry, ~12.8KB
static bool radialPrepared=false;
static uint16_t lcdRow=0;
// 0 = free, 1 = Core0 composing, 2 = ready, 3 = Core1 transmitting to LCD
static uint8_t frameStatus = 0;
static portMUX_TYPE frameMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t frameReadyCount=0, frameShownCount=0, frameDroppedCount=0;
static char lastNameShown[16]="";
static char lastToastShown[24]="";
static uint16_t lastTrackShown=0, lastTotalShown=0;
static bool lastTrackKnownShown=false;
static bool lastReadyShown=false, lastErrorShown=false;
static bool lastToastActive=false, labelBuilt=false, framePainted=false;
static bool frameDirty=true;
static bool lastNoNameShown=false;
static uint32_t lastDraw=0;
static float angle=0, displaySpeed=0;

static uint16_t color565Raw(uint8_t r, uint8_t g, uint8_t b){
  return uint16_t(((uint16_t(r)&0xF8)<<8)|((uint16_t(g)&0xFC)<<3)|(b>>3));
}
static void labelDot(int x,int y,uint16_t c){
  if((unsigned)x<LABEL_W && (unsigned)y<LABEL_W) labelPixels[y*LABEL_W+x]=c;
}
static constexpr char glyphIndex[]=" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_~/";
// Font rows are 5 bits wide, seven rows tall. This font is part of the firmware.
static constexpr uint8_t glyphRows[][7]={
  {0b00000,0b00000,0b00000,0b00000,0b00000,0b00000,0b00000}, // space
  {0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}, // A
  {0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110}, // B
  {0b01111,0b10000,0b10000,0b10000,0b10000,0b10000,0b01111}, // C
  {0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110}, // D
  {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111}, // E
  {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000}, // F
  {0b01111,0b10000,0b10000,0b10111,0b10001,0b10001,0b01111}, // G
  {0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}, // H
  {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b11111}, // I
  {0b00111,0b00010,0b00010,0b00010,0b10010,0b10010,0b01100}, // J
  {0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001}, // K
  {0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111}, // L
  {0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001}, // M
  {0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001}, // N
  {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}, // O
  {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000}, // P
  {0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101}, // Q
  {0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001}, // R
  {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110}, // S
  {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}, // T
  {0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}, // U
  {0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100}, // V
  {0b10001,0b10001,0b10001,0b10101,0b10101,0b11011,0b10001}, // W
  {0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001}, // X
  {0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100}, // Y
  {0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111}, // Z
  {0b01110,0b10011,0b10101,0b10101,0b10101,0b11001,0b01110}, // 0
  {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110}, // 1
  {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111}, // 2
  {0b11110,0b00001,0b00001,0b01110,0b00001,0b00001,0b11110}, // 3
  {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010}, // 4
  {0b11111,0b10000,0b10000,0b11110,0b00001,0b00001,0b11110}, // 5
  {0b01110,0b10000,0b10000,0b11110,0b10001,0b10001,0b01110}, // 6
  {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000}, // 7
  {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110}, // 8
  {0b01110,0b10001,0b10001,0b01111,0b00001,0b00001,0b01110}, // 9
  {0b00000,0b00000,0b00000,0b00000,0b00000,0b01100,0b01100}, // .
  {0b00000,0b00000,0b00000,0b11111,0b00000,0b00000,0b00000}, // -
  {0b00000,0b00000,0b00000,0b00000,0b00000,0b00000,0b11111}, // _
  {0b00000,0b01001,0b10110,0b00000,0b00000,0b00000,0b00000}, // ~
  {0b00001,0b00001,0b00010,0b00100,0b01000,0b10000,0b10000}, // /
};
static_assert(sizeof(glyphRows)/sizeof(glyphRows[0]) == sizeof(glyphIndex)-1,
              "glyph table index length mismatch");
static const uint8_t* glyphFor(char c){
  if(c>='a' && c<='z')c=char(c-'a'+'A');
  for(size_t k=0;k<sizeof(glyphIndex)-1;k++)if(glyphIndex[k]==c)return glyphRows[k];
  return glyphRows[0];
}
static void labelText(const char* txt,int cx,int top,int scale,uint16_t ink){
  const int n=int(strlen(txt));
  const int advance=5*scale+1;
  const int width=n>0?n*advance-1:0;
  int left=cx-width/2;
  for(int i=0;i<n;i++){
    const uint8_t* rows=glyphFor(txt[i]);
    for(int y=0;y<7;y++)for(int x=0;x<5;x++){
      if((rows[y] & (1u<<(4-x)))==0)continue;
      for(int sy=0;sy<scale;sy++)for(int sx=0;sx<scale;sx++)
        labelDot(left+i*advance+x*scale+sx,top+y*scale+sy,ink);
    }
  }
}
static void buildLabel(const DisplayState &state){
  const uint16_t base=color565Raw(210,176,121), edge=color565Raw(151,118,72);
  const uint16_t cream=color565Raw(226,201,153), ink=color565Raw(68,50,31);
  for(int y=0;y<LABEL_W;y++) for(int x=0;x<LABEL_W;x++){
    const int dx=x-61,dy=y-61,r2=dx*dx+dy*dy;
    uint16_t color=color565Raw(22,23,26);
    if(r2<=61*61) color=(r2>58*58)?edge:((r2>55*55)?base:cream);
    if(r2>=54*54 && r2<=55*55) color=edge;
    labelPixels[y*LABEL_W+x]=color;
  }
  // Filename-to-artwork binding is exact and stable; no random art assignment.
  int artwork=-1;
  if(state.nameKnown){
    for(unsigned i=0;i<kLabelMapCount;i++){
      if(memcmp(kLabelMap[i].key,state.name,11)==0){artwork=kLabelMap[i].artwork;break;}
    }
  }
  if(artwork>=0 && artwork<kArtworkCount){
    for(int y=0;y<LABEL_W;y++)for(int x=0;x<LABEL_W;x++){
      const int dx=x-61,dy=y-61;
      if(dx*dx+dy*dy<56*56){labelPixels[y*LABEL_W+x]=kArtwork[artwork][y*LABEL_W+x];}
    }
  }
  labelText("VINYL M5",61,26,2,artwork>=0?color565Raw(245,240,217):ink);
  for(int x=28;x<=94;x++)labelDot(x,43,edge);
  // Rotating center label: currently playing global track / total playable tracks.
  // Never show a fabricated total before N9301 0x0D has replied.
  char number[16];
  if(state.trackKnown){
    if(state.total>0)
      snprintf(number,sizeof(number),"%u/%u",unsigned(state.track),unsigned(state.total));
    else
      snprintf(number,sizeof(number),"%u/--",unsigned(state.track));
  }else{
    if(state.total>0) snprintf(number,sizeof(number),"--/%u",unsigned(state.total));
    else snprintf(number,sizeof(number),"--/--");
  }
  labelText(number,61,44,2,ink);
  char name[16]="LOADING";
  if(state.nameKnown && state.name[0]){
    memcpy(name,state.name,8);name[8]=0;
    for(int i=7;i>=0 && name[i]==' ';--i)name[i]=0;
  }
  // File name and all other lettering are rendered into the rotating texture.
  if(state.fileError){
    labelText("FILE",61,77,2,ink);
    labelText("NOT FOUND",61,92,2,ink);
  } else if(state.uartSilent){
    labelText("NO UART",61,81,2,ink);
  } else if(state.noSD){
    labelText("NO SD",61,81,2,ink);
  } else if(state.nameUnavailable){
    labelText("NO NAME",61,81,2,ink);
  } else {
    labelText(name,61,83,2,artwork>=0?color565Raw(245,240,217):ink);
  }
  for(int y=58;y<=64;y++)for(int x=58;x<=64;x++){
    int dx=x-61,dy=y-61;
    if(dx*dx+dy*dy<=9)labelDot(x,y,color565Raw(32,33,34));
    if(dx*dx+dy*dy<=1)labelDot(x,y,color565Raw(210,210,206));
  }
  labelBuilt=true;
}
// Composite the ENTIRE record, not merely a square patch in its middle.
// All scene pixels and the transient toast are rebuilt in the SAME frame.
// A removed toast therefore never leaves a black rectangle on the vinyl.
static inline uint16_t readLabelClamped(int x,int y){
  if(x<0)x=0; else if(x>=LABEL_W)x=LABEL_W-1;
  if(y<0)y=0; else if(y>=LABEL_W)y=LABEL_W-1;
  return labelPixels[y*LABEL_W+x];
}
static uint16_t sampleLabel(float x,float y){
  // Bilinear RGB565 texture filtering; the old nearest-neighbor sampling
  // caused jagged 5x7 glyphs and a visibly stepping circular label edge.
  int x0=(int)floorf(x), y0=(int)floorf(y);
  int ax=(int)((x-x0)*256.0f), ay=(int)((y-y0)*256.0f);
  if(ax<0)ax=0;
  if(ax>255)ax=255;
  if(ay<0)ay=0;
  if(ay>255)ay=255;
  uint16_t p0=readLabelClamped(x0,y0), p1=readLabelClamped(x0+1,y0);
  uint16_t p2=readLabelClamped(x0,y0+1), p3=readLabelClamped(x0+1,y0+1);
  const int a=(256-ax)*(256-ay), b=ax*(256-ay), c=(256-ax)*ay, d=ax*ay;
  int r=((p0>>11)*a+(p1>>11)*b+(p2>>11)*c+(p3>>11)*d+32768)>>16;
  int g=(((p0>>5)&63)*a+((p1>>5)&63)*b+((p2>>5)&63)*c+((p3>>5)&63)*d+32768)>>16;
  int bl=((p0&31)*a+(p1&31)*b+(p2&31)*c+(p3&31)*d+32768)>>16;
  if(r>31)r=31;
  if(g>63)g=63;
  if(bl>31)bl=31;
  return uint16_t((r<<11)|(g<<5)|bl);
}
static void frameText(const char* message,int cx,int top,int scale,uint16_t ink){
  // Draw using our large embedded glyph font, on the frame, not the LCD.
  const size_t n=strlen(message);
  const int advance=5*scale+scale; // 6*scale pixels/char
  int left=cx-(int(n)*advance-scale)/2;
  for(size_t k=0;k<n;k++){
    const uint8_t* glyph=glyphFor(message[k]);
    for(int y=0;y<7;y++)for(int x=0;x<5;x++){
      if(!(glyph[y]&(1u<<(4-x))))continue;
      for(int yy=0;yy<scale;yy++)for(int xx=0;xx<scale;xx++){
        const int dstX=left+int(k)*advance+x*scale+xx;
        const int dstY=top+y*scale+yy;
        if((unsigned)dstX<FRAME_W && (unsigned)dstY<FRAME_W)
          framePixels[dstY*FRAME_W+dstX]=ink;
      }
    }
  }
}
static void composeFrame(float radians,const DisplayState &state, bool showPopup){
  const float ca=cosf(radians), sa=sinf(radians);
  const uint16_t outside=color565Raw(9,11,14);
  const uint16_t rim=color565Raw(51,53,55);
  const uint16_t black=color565Raw(15,16,19);
  for(int y=0;y<FRAME_W;y++){
    const int dy=y-FRAME_W/2;
    for(int x=0;x<FRAME_W;x++){
      const int dx=x-FRAME_W/2;
      const int idx=y*FRAME_W+x;
      const int r=radialCache[abs(dy)*(FRAME_W/2+1)+abs(dx)];
      const int r2=dx*dx+dy*dy;
      uint16_t out=outside;
      if(r2<=112*112){
        out=rim;
        if(r2<=108*108){
          out=black;
          if(r2>63*63){
            // Rotation-visible reflections within the vinyl grooves.
            // A record composed only of concentric rings appears stationary.
            const float slash=dx*(ca*0.72f-sa*0.69f)+dy*(sa*0.72f+ca*0.69f);
            const float glint=fabsf(slash-15.0f);
            uint8_t k=26;
            if(glint<10.0f) k=uint8_t(40-(int)glint);
            if(slash < -45.0f && slash > -63.0f) k=uint8_t(k+7);
            if((r%5)==0)k=uint8_t(k+7);
            else if((r%5)==1)k=uint8_t(k+3);
            out=color565Raw(k,k+1,k+3);
          }
        }
      }
      if(r2<=61*61 && labelBuilt){
        float u=ca*dx+sa*dy+LABEL_W*0.5f;
        float v=-sa*dx+ca*dy+LABEL_W*0.5f;
        out=sampleLabel(u,v);
      }
      framePixels[idx]=out;
    }
  }
  if(showPopup){
    // Toast is an integral part of the new frame, not a direct-LCD erasure.
    const int top=181, bottom=212, left=31, right=193;
    for(int y=top;y<bottom;y++)for(int x=left;x<right;x++){
      bool border=(y==top||y==bottom-1||x==left||x==right-1);
      framePixels[y*FRAME_W+x]=border?color565Raw(177,149,102):color565Raw(18,21,24);
    }
    char line[14];
    snprintf(line,sizeof(line),"%.13s",state.toast);
    frameText(line,FRAME_W/2,189,2,color565Raw(244,223,185));
  }
}
static void drawBackground(){
  M5Dial.Display.fillScreen(M5Dial.Display.color565(9,11,14));
}
static void prepareRadialCache(){
  if(radialPrepared)return;
  for(int dy=0;dy<=FRAME_W/2;dy++)for(int dx=0;dx<=FRAME_W/2;dx++){
    radialCache[dy*(FRAME_W/2+1)+dx]=(uint8_t)sqrtf(float(dx*dx+dy*dy));
  }
  radialPrepared=true;
}
// Only Core0 composes the RGB565 image; short LCD stripes are serialized against input.
static void render(uint32_t now,const DisplayState &state){
  if(now-lastDraw<42)return; // <=24fps; keep render within LCD throughput
  prepareRadialCache();
  const float dt=fminf(float(now-lastDraw)*0.001f,0.20f);
  const float target=state.playing?3.4906585f:0.0f;
  displaySpeed+=(target-displaySpeed)*fminf(1.0f,dt*5.0f);
  if(fabsf(displaySpeed)<0.005f)displaySpeed=0;
  angle+=displaySpeed*dt;
  if(angle>=6.2831853f)angle-=6.2831853f;
  const bool labelChanged=(!labelBuilt||lastTrackShown!=state.track||
          lastTotalShown!=state.total||lastTrackKnownShown!=state.trackKnown||
          lastReadyShown!=state.nameKnown||lastErrorShown!=state.fileError||
          lastNoNameShown!=state.nameUnavailable||strcmp(lastNameShown,state.name)!=0);
  if(labelChanged){
    buildLabel(state);
    lastTrackShown=state.track;lastTotalShown=state.total;
    lastTrackKnownShown=state.trackKnown;lastReadyShown=state.nameKnown;
    lastErrorShown=state.fileError;
    lastNoNameShown=state.nameUnavailable;
    snprintf(lastNameShown,sizeof(lastNameShown),"%s",state.name);
  }
  const bool showPopup=(int32_t)(state.popupUntil-now)>0 && state.toast[0];
  const bool toastChanged=showPopup!=lastToastActive||strcmp(lastToastShown,state.toast)!=0;
  if(labelChanged||toastChanged) frameDirty=true;
  if(frameDirty||!framePainted||displaySpeed!=0){
    bool acquired=false;
    portENTER_CRITICAL(&frameMux);
    if(frameStatus==0){frameStatus=1;acquired=true;}
    else ++frameDroppedCount;
    portEXIT_CRITICAL(&frameMux);
    if(acquired){
      composeFrame(angle,state,showPopup);
      portENTER_CRITICAL(&frameMux);
      frameStatus=2;
      ++frameReadyCount;
      portEXIT_CRITICAL(&frameMux);
      framePainted=true;
      frameDirty=false;
      lastToastActive=showPopup;
      snprintf(lastToastShown,sizeof(lastToastShown),"%s",state.toast);
    }
  }
  lastDraw=now;
}
// Input and N9301 live on Core1. Image calculation and short LCD stripe
// transfers live on Core0. A FreeRTOS mutex serializes M5 hardware calls.
// R1F21 proved the render path can visibly rotate, but lacked this arbitration.
// Limit the LCD lock to one 16-line stripe to keep touch polling responsive.
static void flushFrameStripOnCore0(){
  portENTER_CRITICAL(&frameMux);
  if(frameStatus==2){frameStatus=3;lcdRow=0;}
  const bool send=(frameStatus==3);
  portEXIT_CRITICAL(&frameMux);
  if(!send) return;
  if(!m5HardwareMutex || xSemaphoreTake(m5HardwareMutex,pdMS_TO_TICKS(5))!=pdTRUE) return;
  constexpr int ROWS_PER_PASS=16;
  const int rows=min(ROWS_PER_PASS,FRAME_W-int(lcdRow));
  auto &d=M5Dial.Display;
  d.startWrite();
  d.pushImage(8,8+lcdRow,FRAME_W,rows,framePixels+lcdRow*FRAME_W);
  d.endWrite();
  xSemaphoreGive(m5HardwareMutex);
  lcdRow+=rows;
  if(lcdRow>=FRAME_W){
    portENTER_CRITICAL(&frameMux);
    frameStatus=0;
    ++frameShownCount;
    portEXIT_CRITICAL(&frameMux);
  }
}
static void displayTask(void *) {
  VINYL_LOGF("[%lu] RENDER / GUARDED LCD core=%d started\n", (unsigned long)millis(), xPortGetCoreID());
  for (;;) {
    DisplayState snapshot;
    portENTER_CRITICAL(&displayMux);
    snapshot=displayShared;
    portEXIT_CRITICAL(&displayMux);
    render(millis(),snapshot);
    flushFrameStripOnCore0();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
static void publishDisplayState(){
  DisplayState snapshot;
  snapshot.playing=playing;
  snapshot.ready=audio.ready;
  // SD detection and the one-second settling delay are NOT missing filenames.
  // Only judge a missing name after a PLAY packet AND actual 0x15 query were
  // transmitted and the module had 10 seconds to answer it.
  const uint32_t elapsedSinceQuery=audio.nameQueryAt ?
      (uint32_t)(millis()-audio.nameQueryAt) : 0;
  // The lack of a 0x15 name reply is a metadata failure, NOT evidence of
  // a garbage audio file. Only report NO NAME after a confirmed playing
  // response and >=10s after an actual filename query.
  const bool decoderConfirmedPlaying=audio.statusKnown && audio.status==1 &&
      audio.lastPlayTx && audio.statusAt>=audio.lastPlayTx;
  snapshot.nameUnavailable=(audio.sdDetectedAt && decoderConfirmedPlaying &&
      !audio.shortNameKnown && !audio.fileError &&
      audio.nameQueryAt>=audio.lastPlayTx && elapsedSinceQuery>=10000);
  snapshot.uartSilent=(audio.ready && audio.rxFrames==0 &&
       (uint32_t)(millis()-audio.bootAt)>=13000);
  snapshot.noSD=(audio.deviceReplyKnown && !audio.present &&
       (uint32_t)(millis()-audio.bootAt)>=13000);
  snapshot.nameKnown=audio.shortNameKnown;
  snapshot.fileError=audio.fileError;
  snapshot.track=audio.track;
  snapshot.total=audio.total;
  snapshot.trackKnown=audio.trackKnown;
  snprintf(snapshot.name,sizeof(snapshot.name),"%s",audio.shortName);
  snprintf(snapshot.toast,sizeof(snapshot.toast),"%s",toast);
  snapshot.popupUntil=popupUntil;
  snapshot.turnAt=lastTurn;
  snapshot.turnSpeed=angularSpeed;
  portENTER_CRITICAL(&displayMux);
  displayShared=snapshot;
  portEXIT_CRITICAL(&displayMux);
}
void setup(){
  Serial.begin(115200);
  delay(100);
  VINYL_LOGF("VINYLM5 R1F27 STARTING reset_reason=%d heap=%u\n",int(esp_reset_reason()),unsigned(ESP.getFreeHeap()));
  auto cfg=M5.config();
  M5Dial.begin(cfg,true,false);
  M5Dial.Display.setRotation(0);
  // Our single frame is host-endian RGB565 (color565Raw), not pre-swapped.
  // Set pushImage byte order explicitly for the 16-bit frame buffer.
  M5Dial.Display.setSwapBytes(true);
  M5Dial.Display.setBrightness(150);
  m5HardwareMutex=xSemaphoreCreateMutex();
  if(!m5HardwareMutex){VINYL_LOGLN("FATAL: cannot create hardware mutex; no render task will start");}
  userPrefs.begin("vinylm5",false);
  restoreTrack=userPrefs.getUShort("track",0);
  audio.volume=userPrefs.getUChar("volume",10);
  if(audio.volume>30)audio.volume=10;
  lastSavedVolume=audio.volume;
  lastSavedTrack=restoreTrack;
  VINYL_LOGF("PREFS restoredTrack=%u volume=%u\n",unsigned(restoreTrack),unsigned(audio.volume));
  VINYL_LOGF("[%lu] raw RGB565 buffers: label=%u frame=%u freeHeap=%u\n",
      (unsigned long)millis(),unsigned(sizeof(labelPixels)),
      unsigned(sizeof(framePixels)),unsigned(ESP.getFreeHeap()));
  drawBackground();
  oldEnc=M5Dial.Encoder.read();
  audio.begin();
  popup("VINYL M5",3000);
  VINYL_LOGLN("VINYLM5 R1F27 BOOT | USB CDC | PORT B RX=1 TX=2 | 9600bps");
  publishDisplayState();
  // Core0 composites and transfers LCD with a mutex; Core1 keeps input priority.
  const BaseType_t created=m5HardwareMutex ? xTaskCreatePinnedToCore(
      displayTask,"vinyl_render",8192,nullptr,1,&displayTaskHandle,0) : pdFAIL;
  VINYL_LOGF("[%10lu] Boot complete, AudioPlayer startup wait=%lu ms, display=%s, displayTask=%s\n",
      (unsigned long)millis(),(unsigned long)STARTUP_MS,"RAW565_CORE0_MUTEX_16ROW",
      created==pdPASS?"OK":"FAILED");
}
void loop(){
  const uint32_t updateStarted=millis();
  // Always prioritize physical input over the next LCD stripe.
  // Do not block forever if the display is using the hardware bus.
  const bool gotHardware=!m5HardwareMutex ||
      (xSemaphoreTake(m5HardwareMutex,pdMS_TO_TICKS(5))==pdTRUE);
  uint32_t now=millis();
  if(gotHardware){
    M5Dial.update();
    now=millis();
    touchUpdate(now);
    encoderUpdate(now);
    if(M5Dial.BtnA.wasPressed()) togglePlay();
    if(m5HardwareMutex) xSemaphoreGive(m5HardwareMutex);
  }
  const uint32_t updateCost=millis()-updateStarted;
  if(updateCost>inputMaxUpdateMs) inputMaxUpdateMs=updateCost;
  ++inputLoopCount;
  // Startup and periodic module-state interrogation.
  static bool configured=false;
  if(audio.ready && !configured && audio.sdDetectedAt &&
     (uint32_t)(now-audio.sdDetectedAt)>=SD_SETTLE_MS){
    configured=true;
    VINYL_LOGF("[%10lu] SD settle completed after %lu ms; configure and PLAY\n",
                (unsigned long)now,(unsigned long)SD_SETTLE_MS);
    audio.clearPending();
    // Configure all-track loop FIRST. Changing mode stops the N9301,
    // therefore never issue this again during normal playback.
    const uint8_t allLoop[] = {0x01,0x00};
    audio.enqueueData(0x0B,allLoop,2);
    audio.enqueue(0x0B,0x00); // read back mode; log confirms 00
    audio.setVolume(audio.volume);
    // Start playback automatically after loop mode and volume are established.
    // Do not require any touchscreen or encoder input at power-on.
    if(restoreTrack>=1 && restoreTrack<65535){
      // Select+PLAY exactly once. 0x06 already plays the selected track.
      // Do NOT follow it with PLAY: that produced audible start/restart.
      audio.selectTrack(restoreTrack);
      VINYL_LOGF("[%10lu] RESTORE+PLAY once track=%u (SD ordering may change)\n",
          (unsigned long)now,unsigned(restoreTrack));
    }else{
      audio.control(0x01); // PLAY once when nothing is saved
      VINYL_LOGF("[%10lu] FIRST PLAY once (no saved track)\n",(unsigned long)now);
    }
    playRequested=true;
    // Start the name-response timeout only after the 0x15 query is sent.
    audio.nameDemandAt=now;
    audio.nameRetries=0;
    audio.lastNameRetryAt=0;
    VINYL_LOGF("[%10lu] AUTO PLAY queued after ALL_LOOP setup\n",(unsigned long)now);
    audio.enqueue(0x04,0x08);
    audio.enqueue(0x04,0x0D);
    audio.enqueue(0x04,0x0E);
    audio.enqueue(0x04,0x15); // request initial 8.3 filename
    audio.enqueue(0x04,0x00); // query actual playback status (do not assume PLAY)
    popup("LOADING",2000);
  }
  trackDialUpdate(now);
  trackMetadataUpdate(now);
  // Retry total count if the module missed the startup query, never more
  // often than 20s; without it the center label shows N/--.
  static uint32_t lastTotalPoll=0;
  if(configured && audio.ready && !audio.total && now-lastTotalPoll>=20000){
    lastTotalPoll=now;
    audio.enqueue(0x04,0x0D);
  }
  audio.update(now);
  if(audio.trackChanged){
    audio.trackChanged=false;
    trackRefreshDue=now+700; // refresh short name on automatic next-track notification
    audio.nameDemandAt=now;
    audio.nameRetries=0;
    audio.lastNameRetryAt=0;
  }
  folderNavigationUpdate(now);
  // Retry only a few missing filename responses. Do not overload the N9301.
  if(audio.ready && audio.nameDemandAt && !audio.shortNameKnown && !audio.fileError &&
     audio.nameRetries<2 && now-audio.nameDemandAt>6500 &&
     audio.nameQueryAt && now-audio.nameQueryAt>=4500 &&
     (!audio.lastNameRetryAt || now-audio.lastNameRetryAt>=5000) && trackRefreshDue==0){
    audio.enqueue(0x04,0x15);
    audio.nameRetries++;
    audio.lastNameRetryAt=now;
    VINYL_LOGF("[%lu] NAME retry=%u\n",(unsigned long)now,unsigned(audio.nameRetries));
  }
  // UART state may be stale or may never be returned despite audible playback.
  // A former STOP notification must never indefinitely lock the disc animation.
  // GPIO audio-BUSY is not connected on PORT.B: exact audible-start detection
  // is impossible; fallback deliberately waits 2.5s AFTER an actual PLAY send.
  if(audio.statusReceived){
    audio.statusReceived=false;
    VINYL_LOGF("[%lu] N9301 RX state=%u sentPlay=%lu statusAt=%lu\n",
      (unsigned long)now,unsigned(audio.status),
      (unsigned long)audio.lastPlayTx,(unsigned long)audio.statusAt);
  }
  const bool confirmedPlaying=audio.statusKnown && audio.status==1 &&
       audio.lastPlayTx && audio.statusAt>=audio.lastPlayTx;
  // Never spin over a KNOWN stopped response received after the play attempt.
  // A missing filename alone does not prove a missing audio file.
  const bool stoppedAfterPlay=audio.statusKnown && audio.status==0 &&
      audio.lastPlayTx && audio.statusAt>=audio.lastPlayTx;
  const bool playbackFallback=audio.lastPlayTx && now-audio.lastPlayTx>=2500 &&
      !stoppedAfterPlay;
  const bool visualPlaying=playRequested && !audio.fileError &&
       (confirmedPlaying || playbackFallback);
  if(visualPlaying!=playing){
    playing=visualPlaying;
    VINYL_LOGF("[%lu] VISUAL playing=%d confirmed=%d fallback=%d\n",
       (unsigned long)now,int(playing),int(confirmedPlaying),int(playbackFallback));
  }
  // Save last confirmed global track/volume, sparingly to protect NVS endurance.
  static uint32_t lastSaveCheck=0;
  if(now-lastSaveCheck>=1000){
    lastSaveCheck=now;
    if(audio.volume!=lastSavedVolume && lastVolumeChanged && now-lastVolumeChanged>3000){
      userPrefs.putUChar("volume",audio.volume);lastSavedVolume=audio.volume;
    }
    if(audio.trackKnown && audio.shortNameKnown &&
       audio.statusKnown && audio.status==1 &&
       audio.track!=lastSavedTrack && now-lastInputAt>5000 &&
       audio.track>=1 && !audio.fileError){
      userPrefs.putUShort("track",audio.track);lastSavedTrack=audio.track;
    }
  }
  // Repair a confirmed unexpected STOP during automatic playback without
  // touching manual STOP or hiding a decoder file-error notification.
  static uint32_t stoppedSince=0, lastAutoRecovery=0;
  if(playRequested && audio.statusKnown && audio.status==0 && audio.lastPlayTx &&
     now-audio.lastPlayTx>6000 && !audio.fileError){
    if(!stoppedSince) stoppedSince=now;
    if(now-stoppedSince>2000 && now-lastAutoRecovery>15000){
      lastAutoRecovery=now;stoppedSince=now;
      if(audio.loopModeKnown && audio.loopMode!=0){
        const uint8_t allLoop[] = {0x01,0x00};
        audio.enqueueData(0x0B,allLoop,2);
        audio.enqueue(0x0B,0x00);
        VINYL_LOGLN("RECOVERY: ALL_LOOP readback wrong; correcting while STOPPED");
      }
      // Prefer NEXT rather than restarting the same stopped track indefinitely.
      audio.control(0x05);
      audio.control(0x01);
      VINYL_LOGLN("RECOVERY: unexpected STOP during requested playback -> NEXT/PLAY");
    }
  } else stoppedSince=0;
  // Read the ALL_LOOP setting without rewriting it during playback.
  static uint32_t lastLoopCheck=0;
  if(audio.ready && configured && (!audio.loopModeKnown || audio.loopMode!=0) &&
     now-lastLoopCheck>=15000){
    lastLoopCheck=now;audio.enqueue(0x0B,0x00);
    if(audio.loopModeKnown && audio.loopMode!=0){
      VINYL_LOGF("LOOP NOT ALL: %u, reconfiguration deferred until STOP\n",unsigned(audio.loopMode));
    }
  }
  // Low-rate readback: keeps rotation tied to real decoder status, and gives
  // a diagnostic trace if the decoder stops at roughly two minutes.
  static uint32_t lastStatusPoll=0;
  if(audio.ready && configured && now-lastStatusPoll>=8000){
    lastStatusPoll=now;
    audio.enqueue(0x04,0x00);
  }
  static uint32_t lastHeartbeat=0;
  if(now-lastHeartbeat>=10000){
    lastHeartbeat=now;
    VINYL_LOGF("[%10lu] HEARTBEAT ready=%u sd=%u config=%u displayPlaying=%u n9301Status=%u track=%u loop=%u rxFrames=%lu txFrames=%lu checksumErr=%lu queue=%u\n",
        (unsigned long)now,(unsigned)audio.ready,(unsigned)audio.present,(unsigned)configured,(unsigned)playing,(unsigned)audio.status,
        (unsigned)audio.track,(unsigned)audio.loopMode,(unsigned long)audio.rxFrames,(unsigned long)audio.txFrames,
        (unsigned long)audio.crcErrors,(unsigned)((audio.tail+16-audio.head)%16));
  }
  publishDisplayState();
  // No LCD transfer on the Core1 control loop. Core0 does the short
  // hardware-mutex-protected stripes and yields between them.
  static uint32_t lastInputHealth=0;
  if(now-lastInputHealth>=3000){
    lastInputHealth=now;
    VINYL_LOGF("[%lu] INPUT_HEALTH polls=%lu maxUpdateMs=%lu uartRx=%lu nameDemand=%lu nameSent=%lu frame=%lu/%lu\n",
      (unsigned long)now,(unsigned long)inputLoopCount,(unsigned long)inputMaxUpdateMs,
      (unsigned long)audio.rxFrames,(unsigned long)audio.nameDemandAt,
      (unsigned long)audio.nameQueryAt,(unsigned long)frameShownCount,(unsigned long)frameReadyCount);
    inputLoopCount=0;inputMaxUpdateMs=0;
  }
  delay(1); // next input poll runs independently of Core0 LCD stripes
}
