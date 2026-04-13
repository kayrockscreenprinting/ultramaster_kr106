// kr106-scope.js — Oscilloscope display (all modes)
// Depends on: kr106-params.js (P, paramValues, synth, vcfDacToHz, etc.)
// Depends on: kr106-controls.js (drawClipLED)
// Uses global: ctx (canvas 2d context, set by kr106-ui.js)

const scopeRing=new Float32Array(4096);
const scopeSyncRing=new Float32Array(4096);
let scopeRingWP=0, scopeSamplesAvail=0, scopeLocalReadPos=0;
const scopeRingR=new Float32Array(4096);
const scopeDisplay=new Float32Array(4096);
const scopeDisplayR=new Float32Array(4096);
let scopeDisplayLen=0, scopeHasData=false, scopeClipHold=0;
let scopeMode=0;
const SCOPE_MODES=6; // 0=wave, 1=spectrum, 2=adsr, 3=vcf, 4=patchbank, 5=about

// Patch bank state
const kPBCols=16, kPBRows=8, kPBCell=8;
const kPBGridH=kPBRows*kPBCell; // 64px
let pbDragging=false, pbDragOriginX=0, pbDragOriginY=0, pbDragEndX=0, pbDragEndY=0;
let pbBallActive=false, pbBallCellX=0, pbBallCellY=0;
let pbBallDX=0, pbBallDY=0, pbBallSpeed=0, pbBallAccum=0, pbBallErrX=0, pbBallErrY=0;
let pbNavHover=-1, pbHover=-1;

function cycleScopeMode(dir){
  scopeMode=((scopeMode+dir)%SCOPE_MODES+SCOPE_MODES)%SCOPE_MODES;
}

// Scope bounds (logical pixels)
const SC={x:790,y:7,w:128,h:74};
const kNavH=10; // nav bar height at bottom
const kNavArrowW=14; // click zone for arrows
let navHover=-1; // 0=left, 1=right, -1=none
let scopeNavPeakDb=-100, scopeNavVcfHz=0;

// Interactive scope drag state
let scopeDragMode=0; // 0=none, 1=volume, 2=vcf, 3=adsr
let scopeDragStartVal=[0,0];
let scopeDragStartY=0, scopeDragStartX=0;
let adsrDragParam=-1;
// ADSR boundary positions (normalized 0-1), cached by drawScopeADSR
let adsrBoundAD=0, adsrBoundDS=0, adsrBoundSR=0, adsrSustainY=0.5;

function updateScopeData(){
if(!synth.ready)return;
const sd=synth.getScopeData();if(!sd)return;

// Two formats: worklet sends {ringL, ringR, sync, writePos}
//              ScriptProcessor sends {heap, ringOff, ringROff, syncOff, writePos}
let len, srcL, srcR, srcS;
if(sd.ringL){
  // AudioWorklet: pre-copied arrays (no race condition)
  len=sd.writePos;
  srcL=sd.ringL; srcR=sd.ringR; srcS=sd.sync;
}else if(sd.heap){
  // ScriptProcessor: read from WASM heap
  if(sd.writePos===0)return;
  len=sd.writePos;
  srcL={get:(i)=>sd.heap[sd.ringOff+i]};
  srcR={get:(i)=>sd.heap[sd.ringROff+i]};
  srcS={get:(i)=>sd.heap[sd.syncOff+i]};
  // Use array-like access below
}else return;

if(len===0)return;

let peak=0;
for(let i=0;i<len;i++){
  const s=srcL[i]!==undefined?srcL[i]:srcL.get(i);
  if(Math.abs(s)>peak)peak=Math.abs(s);
  scopeRing[i]=s;
  scopeRingR[i]=srcR[i]!==undefined?srcR[i]:srcR.get(i);
  scopeSyncRing[i]=srcS[i]!==undefined?srcS[i]:srcS.get(i)}
scopeRingWP=len;
scopeSamplesAvail=len;

// Signal consumed (ScriptProcessor double-buffer only)
if(!sd.ringL&&synth.mod&&synth.mod._kr106_scope_consumed)synth.mod._kr106_scope_consumed();

if(peak<1e-6){scopeHasData=false;return}

// Find two consecutive sync pulses (search backward from end)
let endDist=-1,startDist=-1;
for(let s=1;s<=len;s++){
  const idx=len-s;
  if(scopeSyncRing[idx]>0){if(endDist<0)endDist=s;else{startDist=s;break}}}

if(startDist>0&&endDist>0){
  const period=startDist-endDist;
  if(period>1){
    scopeDisplayLen=period;
    const si=len-startDist;
    for(let i=0;i<period;i++){scopeDisplay[i]=scopeRing[si+i];scopeDisplayR[i]=scopeRingR[si+i]}
    scopeHasData=true}}}

// ===== Shared drawing helpers =====

function scopeCrosshairs(sx,sy,w,h){
  ctx.fillStyle='#004000';
  const cx=Math.round(w*0.5),cy=Math.round(h*0.5);
  ctx.fillRect(sx+cx,sy,1,h);
  ctx.fillRect(sx,sy+cy,w,1);
  for(let i=5;;i+=5){
    let any=false;
    if(cx+i<w){ctx.fillRect(sx+cx+i,sy+cy-1,1,3);any=true}
    if(cx-i>=0){ctx.fillRect(sx+cx-i,sy+cy-1,1,3);any=true}
    if(!any)break}
  for(let i=5;;i+=5){
    let any=false;
    if(cy+i<h){ctx.fillRect(sx+cx-1,sy+cy+i,3,1);any=true}
    if(cy-i>=0){ctx.fillRect(sx+cx-1,sy+cy-i,3,1);any=true}
    if(!any)break}
}

function scopePeakReadout(sx,sy,w,h,peakL){
  const peakDb=peakL>1e-10?20*Math.log10(peakL):-200;
  if(peakDb>=0)scopeClipHold=30;else if(scopeClipHold>0)scopeClipHold--;
  ctx.fillStyle=scopeClipHold>0?'#00ff00':'#008000';
  ctx.font=(10/S)+'px monospace';
  ctx.textAlign='right';
  ctx.fillText(peakDb.toFixed(1)+' dB',sx+w-2,sy+h-3);
  ctx.textAlign='left';
}

// ===== Mode 0: Waveform =====

function drawScopeWaveform(ch){
  const{x:sx,y:sy,w}=SC;const h=ch;const v2=h/2|0;

  scopeCrosshairs(sx,sy,w,h);

  if(!scopeHasData||scopeDisplayLen<2)return;

  function interpY(buf,px){
    const pos=px/w*scopeDisplayLen;
    let s0=pos|0,frac=pos-s0;
    if(s0>=scopeDisplayLen-1){s0=scopeDisplayLen-2;frac=1}
    return (buf[s0]+frac*(buf[s0+1]-buf[s0]))*-v2+v2}

  // R channel (dim, 1.5px path)
  ctx.beginPath();
  ctx.moveTo(sx,sy+interpY(scopeDisplayR,0));
  for(let px=0.5;px<w;px+=0.5)ctx.lineTo(sx+px,sy+interpY(scopeDisplayR,px));
  ctx.strokeStyle='#008000';ctx.lineWidth=1.5;ctx.stroke();

  // L channel (bright, 1.5px path) + peak measurement
  let peakL=0;
  ctx.beginPath();
  ctx.moveTo(sx,sy+interpY(scopeDisplay,0));
  for(let px=0.5;px<w;px+=0.5){
    const y=interpY(scopeDisplay,px);
    ctx.lineTo(sx+px,sy+y);
    const sample=(v2-y)/v2;
    if(Math.abs(sample)>peakL)peakL=Math.abs(sample)}
  ctx.strokeStyle='#00ff00';ctx.lineWidth=1.5;ctx.stroke();

  scopeNavPeakDb=peakL>1e-10?20*Math.log10(peakL):-200;
  if(scopeNavPeakDb>=0)scopeClipHold=30;else if(scopeClipHold>0)scopeClipHold--;
}

// ===== Mode 1: Spectrum (FFT) =====

// Simple radix-2 FFT (in-place, Cooley-Tukey)
const FFT_SIZE=4096;
const fftReal=new Float32Array(FFT_SIZE);
const fftImag=new Float32Array(FFT_SIZE);
let hannWindow=null;

function initHann(){
  if(hannWindow)return;
  hannWindow=new Float32Array(FFT_SIZE);
  for(let i=0;i<FFT_SIZE;i++)hannWindow[i]=0.5*(1-Math.cos(2*Math.PI*i/FFT_SIZE));
}

function fft(re,im,n){
  // Bit-reversal permutation
  for(let i=1,j=0;i<n;i++){
    let bit=n>>1;
    for(;j&bit;bit>>=1)j^=bit;
    j^=bit;
    if(i<j){let t=re[i];re[i]=re[j];re[j]=t;t=im[i];im[i]=im[j];im[j]=t}}
  // Butterfly
  for(let len=2;len<=n;len*=2){
    const ang=-2*Math.PI/len;
    const wRe=Math.cos(ang),wIm=Math.sin(ang);
    for(let i=0;i<n;i+=len){
      let curRe=1,curIm=0;
      for(let j=0;j<len/2;j++){
        const tRe=re[i+j+len/2]*curRe-im[i+j+len/2]*curIm;
        const tIm=re[i+j+len/2]*curIm+im[i+j+len/2]*curRe;
        re[i+j+len/2]=re[i+j]-tRe;im[i+j+len/2]=im[i+j]-tIm;
        re[i+j]+=tRe;im[i+j]+=tIm;
        const nRe=curRe*wRe-curIm*wIm;curIm=curRe*wIm+curIm*wRe;curRe=nRe}}}
}

function drawScopeSpectrum(ch){
  initHann();
  const{x:sx,y:sy,w}=SC;const h=ch;

  // Fill FFT buffer from ring (most recent 4096 samples)
  for(let i=0;i<FFT_SIZE;i++){
    const idx=(scopeRingWP-FFT_SIZE+i+4096)%4096;
    fftReal[i]=scopeRing[idx]*hannWindow[i];
    fftImag[i]=0}
  fft(fftReal,fftImag,FFT_SIZE);

  const sr=synth.ctx?synth.ctx.sampleRate:44100;
  const numBins=FFT_SIZE/2;
  const minHz=20, maxHz=sr/2;
  const dbMin=-90, dbMax=0;

  // Grid: frequency decades
  ctx.fillStyle='#004000';
  for(let dec=10;dec<=100000;dec*=10){
    if(dec<minHz||dec>maxHz)continue;
    const xp=Math.round(Math.log(dec/minHz)/Math.log(maxHz/minHz)*w);
    ctx.fillRect(sx+xp,sy,1,h);
    // Sub-decade ticks
    for(let s=2;s<=9;s++){
      const f=dec*s;if(f>maxHz)break;
      const xt=Math.round(Math.log(f/minHz)/Math.log(maxHz/minHz)*w);
      ctx.fillRect(sx+xt,sy+h-3,1,3)}}
  // dB grid
  for(let db=dbMin+18;db<dbMax;db+=18){
    const yp=Math.round((1-(db-dbMin)/(dbMax-dbMin))*h);
    ctx.fillRect(sx,sy+yp,w,1)}

  // Spectrum curve (thin path, half-pixel steps like native)
  function specY(px){
    const frac=px/w;
    const freq=minHz*Math.pow(maxHz/minHz,frac);
    const bin=freq*FFT_SIZE/sr;
    const b0=Math.min(Math.floor(bin),numBins-2);
    const bf=bin-b0;
    const mag0=Math.sqrt(fftReal[b0]*fftReal[b0]+fftImag[b0]*fftImag[b0])/numBins;
    const mag1=Math.sqrt(fftReal[b0+1]*fftReal[b0+1]+fftImag[b0+1]*fftImag[b0+1])/numBins;
    const mag=mag0+(mag1-mag0)*bf;
    const db=mag>1e-10?20*Math.log10(mag):-200;
    const yNorm=(db-dbMin)/(dbMax-dbMin);
    return (1-yNorm)*h}

  ctx.beginPath();
  ctx.moveTo(sx,sy+specY(0));
  for(let px=0.5;px<w;px+=0.5)ctx.lineTo(sx+px,sy+specY(px));
  ctx.strokeStyle='#00ff00';
  ctx.lineWidth=1;
  ctx.stroke();
}

// ===== Mode 2: ADSR Envelope =====

// J106 ROM-accurate tables and functions (matching KR106ADSR.h exactly)
const kEnvMax=0x3FFF; // 14-bit envelope maximum
const kTickRate=1000/4.2; // ~238.1 Hz

// Generate decay/release table (matches GenerateDecRelTable in KR106ADSR.h)
const kDecRelTable=(function(){
  const counts=[4,1,10,28,22,58,4];
  const steps=[0x2000,0x1000,0x0800,0x0080,0x000C,0x0004,0x0001];
  const t=new Uint16Array(128);
  let val=0x1000,i=0;
  t[i++]=val;
  for(let seg=0;seg<7;seg++)
    for(let n=0;n<counts[seg];n++)
      t[i++]=(val=(val+steps[seg])&0xFFFF);
  return t})();

// ROM-accurate attack increment (matches AttackIncFromSlider in KR106ADSR.h)
function adsrAtkInc(slider){
  const s=Math.max(0,Math.min(1,slider));
  if(s<0.003937)return kEnvMax;
  if(s<=0.5)return Math.round(8192/(s*127));
  if(s<=0.681102)return Math.round(305.03-352.26*s);
  if(s<=0.846457)return Math.round(194.74-190.50*s);
  if(s<=0.956693)return Math.round(86.37-62.52*s);
  return Math.max(1,Math.round(148-127*s))}

// ROM-accurate CalcDecay (matches D7811G $083D: 3 partial products, drops VL*CL)
function adsrCalcDecay(value,coeff){
  const vh=(value>>8)&0xFF,vl=value&0xFF;
  const ch=(coeff>>8)&0xFF,cl=coeff&0xFF;
  return (vh*ch)+((vh*cl)>>8)+((vl*ch)>>8)}

// J6 envelope helpers (matching KR106Scope.h paintADSR exactly)
function j6AtkTau(v){return 0.001500*Math.exp(11.7382*v-4.7207*v*v)}
function j6DRTau(v){return 0.003577*Math.exp(12.946*v-5.0638*v*v)}

function drawScopeADSR(ch){
  const{x:sx,y:sy,w}=SC;const h=ch;
  const a=paramValues[P.envA],d=paramValues[P.envD];
  const sustain=Math.max(paramValues[P.envS],0.001),r=paramValues[P.envR];
  const isJ106=paramValues[P.adsrMode]>=0.5;

  const kSustainMs=1000;
  const kThreshold=0.01;

  let attackMs,decayMs,releaseMs;
  // Simulation sample rate (high enough for smooth curve)
  const simRate=1000; // 1 sample per ms
  let curve;

  if(isJ106){
    // J106: ROM-accurate integer simulation
    const atkInc=adsrAtkInc(a);
    const decIdx=Math.max(0,Math.min(127,Math.round(d*127)));
    const relIdx=Math.max(0,Math.min(127,Math.round(r*127)));
    const decCoeff=kDecRelTable[decIdx];
    const relCoeff=kDecRelTable[relIdx];
    const susI=Math.round(sustain*kEnvMax);

    attackMs=1+(Math.ceil(kEnvMax/atkInc)-1)*(1000/kTickRate);

    {let diff=kEnvMax-susI,ticks=0;
     while(diff>kEnvMax*kThreshold&&ticks<50000){diff=adsrCalcDecay(diff,decCoeff);ticks++}
     decayMs=ticks*1000/kTickRate}

    {let env=susI,ticks=0;
     while(env>kEnvMax*kThreshold&&ticks<50000){env=adsrCalcDecay(env,relCoeff);ticks++}
     releaseMs=ticks*1000/kTickRate}

    const totalMs=attackMs+decayMs+kSustainMs+releaseMs;
    const windowMs=Math.max(500,totalMs);
    const msAD=attackMs,msDS=attackMs+decayMs,msSR=msDS+kSustainMs;

    const maxTicks=Math.floor(windowMs*kTickRate/1000)+2;
    const sustainStartTick=Math.floor(msDS*kTickRate/1000);
    const sustainEndTick=Math.floor(msSR*kTickRate/1000);
    const raw=new Float32Array(maxTicks+1);
    let envI=0,attacking=true;
    for(let t=0;t<=maxTicks;t++){
      raw[t]=envI/kEnvMax;
      if(t>=sustainEndTick){envI=adsrCalcDecay(envI,relCoeff)}
      else if(t>=sustainStartTick){envI=susI}
      else if(attacking){
        const sum=envI+atkInc;
        if(sum>=kEnvMax){envI=kEnvMax;attacking=false}
        else envI=sum}
      else{
        if(envI>susI){envI=adsrCalcDecay(envI-susI,decCoeff)+susI}
        else envI=susI}}

    // Resample to ms resolution
    const numMs=Math.ceil(windowMs);
    curve=new Float32Array(numMs+1);
    for(let ms=0;ms<=numMs;ms++){
      const tickF=ms*kTickRate/1000;
      const t0=Math.min(Math.floor(tickF),maxTicks-1);
      const frac=tickF-t0;
      curve[ms]=raw[t0]+(raw[Math.min(t0+1,maxTicks)]-raw[t0])*frac}
  }else{
    // J60/J6: analog RC exponential envelope
    const atkTau=j6AtkTau(a);
    const decTau=j6DRTau(d);
    const relTau=j6DRTau(r);
    const kTarget=1.2; // RC overshoot target

    // Attack: RC toward 1.2, complete when env >= 1.0
    attackMs=atkTau>0?-atkTau*Math.log(1-(1/(kTarget)))*1000:1;

    // Decay: RC from 1.0 toward sustain
    if(sustain<0.99){
      decayMs=-decTau*Math.log(kThreshold/(1-sustain))*1000;
    }else decayMs=0;

    // Release: RC from sustain toward 0
    releaseMs=sustain>kThreshold?-relTau*Math.log(kThreshold/sustain)*1000:0;

    const totalMs=attackMs+decayMs+kSustainMs+releaseMs;
    const windowMs=Math.max(500,totalMs);
    const msAD=attackMs,msDS=attackMs+decayMs,msSR=msDS+kSustainMs;

    const numMs=Math.ceil(windowMs);
    curve=new Float32Array(numMs+1);
    const dt=0.001; // 1ms step
    for(let ms=0;ms<=numMs;ms++){
      const t=ms*dt;
      let env;
      if(ms<=attackMs){
        // Attack: RC charge toward kTarget
        const coeff=atkTau>0?Math.exp(-dt/atkTau):0;
        if(ms===0)env=0;
        else{
          const prev=curve[ms-1];
          env=prev+(kTarget-prev)*(1-Math.exp(-dt/atkTau));
          if(env>1)env=1}
      }else if(ms<=msDS){
        // Decay: RC discharge toward sustain
        const elapsed=(ms-attackMs)*dt;
        env=sustain+(1-sustain)*Math.exp(-elapsed/decTau);
      }else if(ms<=msSR){
        env=sustain;
      }else{
        // Release: RC discharge toward 0
        const elapsed=(ms-msSR)*dt;
        env=sustain*Math.exp(-elapsed/relTau);
      }
      curve[ms]=Math.max(0,Math.min(1,env));
    }
  }

  // Common drawing code
  const totalMs=attackMs+decayMs+kSustainMs+releaseMs;
  const windowMs=Math.max(500,totalMs);
  const msAD=attackMs,msDS=attackMs+decayMs,msSR=msDS+kSustainMs;

  // Cache for interactive drag
  adsrBoundAD=msAD/windowMs;
  adsrBoundDS=msDS/windowMs;
  adsrBoundSR=msSR/windowMs;
  adsrSustainY=1-sustain;

  // Bottom tick marks
  ctx.fillStyle='#004000';
  const numSecs=Math.floor(windowMs/1000);
  for(let s=1;s<=numSecs;s++){
    const tx=Math.round(s*1000/windowMs*(w-1));
    if(tx>0&&tx<w)ctx.fillRect(sx+tx,sy+h-3,1,3)}

  // Phase boundary lines
  function drawBound(ms){
    const x=Math.round(ms/windowMs*(w-1));
    if(x>0&&x<w-1)ctx.fillRect(sx+x,sy,1,h)}
  drawBound(msAD);drawBound(msDS);drawBound(msSR);

  // Draw curve
  ctx.beginPath();
  const numMs=curve.length-1;
  function envAtPx(px){
    const ms=(px/(w-1))*windowMs;
    const ms0=Math.min(Math.floor(ms),numMs-1);
    const frac=ms-ms0;
    const env=curve[ms0]+(curve[Math.min(ms0+1,numMs)]-curve[ms0])*frac;
    return 1+(1-Math.max(0,Math.min(1,env)))*(h-3)}
  ctx.moveTo(sx,sy+envAtPx(0));
  for(let px=0.5;px<w;px+=0.5)ctx.lineTo(sx+px,sy+envAtPx(px));
  ctx.strokeStyle='#00ff00';ctx.lineWidth=1.5;ctx.stroke();
}

// ===== Mode 3: Patch Bank (16x8 grid + nav + bouncing ball) =====

function pbPatchAt(x,y){
  if(y>=kPBGridH)return -1;
  const c=Math.floor(x/kPBCell),r=Math.floor(y/kPBCell);
  if(c<0||c>=kPBCols||r<0||r>=kPBRows)return -1;
  const idx=r*kPBCols+c;
  return idx<128?idx:-1}

function pbUpdateBall(){
  if(!pbBallActive)return;
  pbBallAccum+=pbBallSpeed;
  if(pbBallAccum<1)return;
  pbBallAccum-=1;

  pbBallErrX+=Math.abs(pbBallDX);
  pbBallErrY+=Math.abs(pbBallDY);
  const sx=pbBallDX>=0?1:-1, sy=pbBallDY>=0?1:-1;
  let cx=pbBallCellX, cy=pbBallCellY;

  if(pbBallErrX>=pbBallErrY){
    cx+=sx;pbBallErrX-=1;
    if(pbBallErrY>=0.5){cy+=sy;pbBallErrY-=1}
  }else{
    cy+=sy;pbBallErrY-=1;
    if(pbBallErrX>=0.5){cx+=sx;pbBallErrX-=1}}

  if(cx<0){cx=1;pbBallDX=-pbBallDX}
  if(cx>kPBCols-1){cx=kPBCols-2;pbBallDX=-pbBallDX}
  if(cy<0){cy=1;pbBallDY=-pbBallDY}
  if(cy>kPBRows-1){cy=kPBRows-2;pbBallDY=-pbBallDY}

  pbBallCellX=cx;pbBallCellY=cy;
  const idx=cy*kPBCols+cx;
  if(idx>=0&&idx<128){currentPreset=idx;synth.loadPreset(idx);syncFP(idx);drawPresetDisplay()}}

function drawScopePatchBank(ch){
  const{x:sx,y:sy,w}=SC;const h=ch;
  const cur=currentPreset;

  // Grid lines
  ctx.fillStyle='#004000';
  for(let c=1;c<kPBCols;c++)ctx.fillRect(sx+c*kPBCell,sy,1,kPBGridH);
  for(let r=1;r<=kPBRows;r++)ctx.fillRect(sx,sy+r*kPBCell,kPBCols*kPBCell,1);

  // Current patch cell (bright green fill)
  if(cur>=0&&cur<128){
    const cc=cur%kPBCols, cr=Math.floor(cur/kPBCols);
    ctx.fillStyle='#00ff00';
    ctx.fillRect(sx+cc*kPBCell+1,sy+cr*kPBCell+1,kPBCell-1,kPBCell-1)}

  // Drag vector (Bresenham on grid, mid green)
  if(pbDragging){
    let c0=Math.floor(pbDragOriginX/kPBCell),r0=Math.floor(pbDragOriginY/kPBCell);
    const c1=Math.max(0,Math.min(kPBCols-1,Math.floor(pbDragEndX/kPBCell)));
    const r1=Math.max(0,Math.min(kPBRows-1,Math.floor(pbDragEndY/kPBCell)));
    let dx=Math.abs(c1-c0),dy=-Math.abs(r1-r0);
    const bsx=c0<c1?1:-1,bsy=r0<r1?1:-1;
    let err=dx+dy;
    ctx.fillStyle='#00c000';
    for(;;){
      const idx=r0*kPBCols+c0;
      if(idx!==cur)ctx.fillRect(sx+c0*kPBCell+1,sy+r0*kPBCell+1,kPBCell-1,kPBCell-1);
      if(c0===c1&&r0===r1)break;
      const e2=2*err;
      if(e2>=dy){err+=dy;c0+=bsx}
      if(e2<=dx){err+=dx;r0+=bsy}}}

  // Ball (bright green cell)
  if(pbBallActive){
    ctx.fillStyle='#00ff00';
    ctx.fillRect(sx+pbBallCellX*kPBCell+1,sy+pbBallCellY*kPBCell+1,kPBCell-1,kPBCell-1)}

  // Update ball each frame
  pbUpdateBall();

}

// ===== Mode 4: VCF Frequency Response =====

// VCF helper functions (matching KR106VCF.h exactly)
function vcfResK(res){
  // ResK_J6 (also used for J106 currently)
  return 0.811*(Math.exp(2.128*res)-1)}
function vcfSoftClipK(k){
  if(k>3){const excess=k-3;k=3+excess/(1+excess*0.2)}
  return Math.min(k,6.6)}
function vcfFreqComp(k,frq){
  const lowQ=2.004*Math.pow(Math.max(frq,1e-5),0.162);
  const blend=Math.min(k*k*0.0625,1);
  return lowQ+blend*(1-lowQ)}
function vcfInputComp(k){return 0.252+0.058*k}

function drawScopeVCF(ch){
  const{x:sx,y:sy,w}=SC;const h=ch;
  const j6=paramValues[P.adsrMode]<0.5;
  const freqSlider=paramValues[P.vcfFreq];
  const resSlider=paramValues[P.vcfRes];

  // Compute cutoff frequency (slider value, before compensation)
  const fcSlider=_pv('_kr106_vcf_freq_hz',freqSlider,j6);

  // Compute resonance K (same curve for both modes currently)
  let k=vcfResK(resSlider);
  k=vcfSoftClipK(k);

  // Apply frequency compensation (corrects cascade droop)
  const sr=synth.ctx?synth.ctx.sampleRate:44100;
  const frqNorm=fcSlider/sr;
  const fc=fcSlider*vcfFreqComp(k,frqNorm);

  // Input compensation + output gain
  const comp=vcfInputComp(k);
  const outGain=4.85;
  const totalComp=comp*outGain;

  // Display range
  const fMin=5,fMax=50000;
  const dbMin=-48,dbMax=24;
  const logMin=Math.log10(fMin),logMax=Math.log10(fMax),logRange=logMax-logMin;

  // Grid: frequency decades
  ctx.fillStyle='#004000';
  for(let dec=10;dec<=100000;dec*=10){
    if(dec<fMin||dec>fMax)continue;
    const xp=Math.round((Math.log10(dec)-logMin)/logRange*(w-1));
    ctx.fillRect(sx+xp,sy,1,h);
    for(let s=2;s<=9;s++){
      const f=dec*s;if(f>fMax)break;
      const xt=Math.round((Math.log10(f)-logMin)/logRange*(w-1));
      ctx.fillRect(sx+xt,sy+h-3,1,3)}}
  // dB grid every 12 dB
  for(let db=dbMin+12;db<=dbMax;db+=12){
    const yp=Math.round((1-(db-dbMin)/(dbMax-dbMin))*h);
    ctx.fillStyle=(db===0)?'#008000':'#004000';
    ctx.fillRect(sx,sy+yp,w,1)}

  // Cutoff marker (dim dashed line at slider frequency, before compensation)
  if(fcSlider>=fMin&&fcSlider<=fMax){
    const fcX=Math.round((Math.log10(fcSlider)-logMin)/logRange*(w-1));
    ctx.fillStyle='#008000';
    ctx.fillRect(sx+fcX,sy,1,h)}

  // Cache for nav label
  scopeNavVcfHz=fcSlider;

  // Transfer function: 4-pole cascade with feedback, normalized to 0 dB at DC
  const k2=k*k;
  const dcDenomSq=(1+k)*(1+k);
  function vcfY(px){
    const logF=logMin+px/(w-1)*logRange;
    const freq=Math.pow(10,logF);
    const x=freq/fc;
    const x2=x*x;
    const p2=1+x2;
    const p4=p2*p2;
    const p8=p4*p4;
    const theta4=4*Math.atan(x);
    const denomSq=p8+2*k*p4*Math.cos(theta4)+k2;
    const magSq=dcDenomSq/denomSq;
    const db=10*Math.log10(Math.max(magSq,1e-12));
    const dbClamped=Math.max(dbMin-12,Math.min(dbMax,db));
    return (1-(dbClamped-dbMin)/(dbMax-dbMin))*(h-1)}

  ctx.beginPath();
  ctx.moveTo(sx,sy+vcfY(0));
  for(let px=0.5;px<w;px+=0.5)ctx.lineTo(sx+px,sy+vcfY(px));
  ctx.strokeStyle='#00ff00';
  ctx.lineWidth=1.5;
  ctx.stroke();
}

// ===== Mode 5: About =====

// Simple vector font glyphs (5x7 grid, stored as 7 rows of 5-bit bitmasks)
const FONT={
  A:[0x0E,0x11,0x11,0x1F,0x11,0x11,0x11],B:[0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E],
  C:[0x0E,0x11,0x10,0x10,0x10,0x11,0x0E],D:[0x1C,0x12,0x11,0x11,0x11,0x12,0x1C],
  E:[0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F],F:[0x1F,0x10,0x10,0x1E,0x10,0x10,0x10],
  G:[0x0E,0x11,0x10,0x17,0x11,0x11,0x0F],H:[0x11,0x11,0x11,0x1F,0x11,0x11,0x11],
  I:[0x0E,0x04,0x04,0x04,0x04,0x04,0x0E],K:[0x11,0x12,0x14,0x18,0x14,0x12,0x11],
  L:[0x10,0x10,0x10,0x10,0x10,0x10,0x1F],M:[0x11,0x1B,0x15,0x15,0x11,0x11,0x11],
  N:[0x11,0x11,0x19,0x15,0x13,0x11,0x11],O:[0x0E,0x11,0x11,0x11,0x11,0x11,0x0E],
  P:[0x1E,0x11,0x11,0x1E,0x10,0x10,0x10],R:[0x1E,0x11,0x11,0x1E,0x14,0x12,0x11],
  S:[0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E],T:[0x1F,0x04,0x04,0x04,0x04,0x04,0x04],
  U:[0x11,0x11,0x11,0x11,0x11,0x11,0x0E],V:[0x11,0x11,0x11,0x11,0x0A,0x0A,0x04],
  W:[0x11,0x11,0x11,0x15,0x15,0x1B,0x11],X:[0x11,0x11,0x0A,0x04,0x0A,0x11,0x11],
  '-':[0x00,0x00,0x00,0x1F,0x00,0x00,0x00],'.':[0x00,0x00,0x00,0x00,0x00,0x00,0x04],
  '0':[0x0E,0x11,0x13,0x15,0x19,0x11,0x0E],'1':[0x04,0x0C,0x04,0x04,0x04,0x04,0x0E],
  '2':[0x0E,0x11,0x01,0x02,0x04,0x08,0x1F],'3':[0x0E,0x11,0x01,0x06,0x01,0x11,0x0E],
  '4':[0x02,0x06,0x0A,0x12,0x1F,0x02,0x02],'5':[0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E],
  '6':[0x06,0x08,0x10,0x1E,0x11,0x11,0x0E],'7':[0x1F,0x01,0x02,0x04,0x08,0x08,0x08],
  '8':[0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E],'9':[0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C],
  ' ':[0x00,0x00,0x00,0x00,0x00,0x00,0x00]};

let aboutPixels=null;
let aboutFrame=0;

function buildAboutPixels(w,h){
  // Rasterize text into pixel array
  const lines=['ULTRAMASTER','KR-106','2.5.5']; // UPDATE version here on release
  const pixels=[];
  const lineH=10; // 7px glyph + 3px gap
  const totalH=lines.length*lineH;
  const startY=Math.floor((h-totalH)/2);

  for(let li=0;li<lines.length;li++){
    const text=lines[li];
    const charW=6; // 5px glyph + 1px gap
    const textW=text.length*charW;
    const startX=Math.floor((w-textW)/2);
    const baseY=startY+li*lineH;
    for(let ci=0;ci<text.length;ci++){
      const glyph=FONT[text[ci]];
      if(!glyph)continue;
      for(let row=0;row<7;row++){
        const bits=glyph[row];
        for(let col=0;col<5;col++){
          if(bits&(0x10>>col))
            pixels.push([startX+ci*charW+col,baseY+row])}}}}
  return pixels;
}

function drawScopeAbout(ch){
  const{x:sx,y:sy,w}=SC;const h=ch;

  if(!aboutPixels)aboutPixels=buildAboutPixels(w,h);

  const total=aboutPixels.length;
  if(total===0)return;

  // 3-second sweep cycle at 30fps = 90 frames
  aboutFrame=(aboutFrame+1)%90;
  const beamPos=aboutFrame/90*total;

  for(let i=0;i<total;i++){
    const[px,py]=aboutPixels[i];
    // Brightness based on distance behind beam
    let dist=(beamPos-i+total)%total;
    const brightness=1-0.9*(dist/total);
    const g=Math.round(brightness*255);
    ctx.fillStyle='rgb(0,'+g+',0)';
    ctx.fillRect(sx+px,sy+py,1,1)}

  // Beam head: 3x3 bloom + white center
  const bi=Math.floor(beamPos);
  if(bi<total){
    const[bx,by]=aboutPixels[bi];
    ctx.fillStyle='#888';
    ctx.fillRect(sx+bx-1,sy+by-1,3,3);
    ctx.fillStyle='#fff';
    ctx.fillRect(sx+bx,sy+by,1,1)}
}

// ===== Nav bar (shared across all modes) =====

function scopeNavLabel(){
  switch(scopeMode){
    case 0:{
      let s='WAVEFORM';
      if(scopeNavPeakDb>-100) s+=' '+scopeNavPeakDb.toFixed(1).padStart(6)+' dB';
      return s}
    case 1:return 'SPECTROGRAPH';
    case 2:return 'ENVELOPE';
    case 3:{
      let s='VCF';
      if(scopeNavVcfHz>0){
        let hz=scopeNavVcfHz>=1000?(scopeNavVcfHz/1000).toFixed(2)+' kHz':Math.round(scopeNavVcfHz)+' Hz';
        s+=' '+hz.padStart(9)}
      return s}
    case 4:return 'PATCH BANK';
    case 5:return 'ABOUT';
    default:return ''}
}

function drawScopeNavBar(){
  const{x:sx,y:sy,w,h}=SC;
  const navY=h-kNavH;

  // Hover highlight
  if(navHover>=0){
    ctx.fillStyle='#002800';
    if(navHover===0) ctx.fillRect(sx,sy+navY,kNavArrowW,kNavH);
    else ctx.fillRect(sx+w-kNavArrowW,sy+navY,kNavArrowW,kNavH)}

  // Top border
  ctx.fillStyle='#004000';
  ctx.fillRect(sx,sy+navY,w,1);

  // < arrow
  ctx.strokeStyle='#008000';ctx.lineWidth=1;
  const ay=navY+kNavH/2;
  ctx.beginPath();ctx.moveTo(sx+5,sy+ay-3);ctx.lineTo(sx+2,sy+ay);ctx.lineTo(sx+5,sy+ay+3);ctx.stroke();

  // > arrow
  ctx.beginPath();ctx.moveTo(sx+w-5,sy+ay-3);ctx.lineTo(sx+w-2,sy+ay);ctx.lineTo(sx+w-5,sy+ay+3);ctx.stroke();

  // Center label
  ctx.fillStyle='#008000';
  ctx.font='8px monospace';
  ctx.textAlign='center';
  ctx.fillText(scopeNavLabel(),sx+w/2,sy+navY+kNavH-2);
  ctx.textAlign='left';
}

// ===== Main drawScope dispatcher =====

function drawScope(){
  if(!synth.ready)return;
  updateScopeData();
  const{x:sx,y:sy,w,h}=SC;
  const ch=h-kNavH;

  ctx.save();
  ctx.beginPath();ctx.rect(sx,sy,w,h);ctx.clip();
  ctx.fillStyle='#000';ctx.fillRect(sx,sy,w,h);

  // Clip content area (above nav bar)
  ctx.save();
  ctx.beginPath();ctx.rect(sx,sy,w,ch);ctx.clip();

  switch(scopeMode){
    case 0:drawScopeWaveform(ch);break;
    case 1:drawScopeSpectrum(ch);break;
    case 2:drawScopeADSR(ch);break;
    case 3:drawScopeVCF(ch);break;
    case 4:drawScopePatchBank(ch);break;
    case 5:drawScopeAbout(ch);break;
  }
  ctx.restore();

  drawScopeNavBar();
  ctx.restore();
}
