// kr106-ui.js — Canvas init, mouse/keyboard handlers, preset sheet, startup
// Depends on: kr106-audio.js, kr106-params.js, kr106-controls.js, kr106-scope.js

let canvas, ctx;

// ===== drawAll =====
function drawAll(){if(!ctx)return;ctx.save();ctx.scale(S,S);if(images.bg)ctx.drawImage(images.bg,0,0,W,H);
const powerOn=paramValues[P.power]>0.5;
for(const c of controls){const[p,t,x,y,e]=c;
if(t==='slider'){const notch=(p===P.hpfFreq)?4:0;drawSlider(x,y,paramValues[p],e||0,notch)}
else if(t==='switch3')drawSwitch(x,y,Math.round(paramValues[p]),3);
else if(t==='switch2')drawSwitch(x,y,Math.round(paramValues[p]),2);
else if(t==='hswitch3')drawHSwitch(x,y,Math.round(paramValues[p]),3);
else if(t==='hswitch2')drawHSwitch(x,y,Math.round(paramValues[p]),2);
else if(t==='knob')drawKnob(x,y,paramValues[p]);
else if(t==='chorusoff')drawBtn(x,y,17,19,0,false);
else if(t==='power')drawPower(x,y,paramValues[p]>0.5);
else if(t==='clipled')drawClipLED(x,y);
else if(t==='bender')drawBender(x,y,paramValues[p]);
else if(t==='btnled')drawBtnLED(x,y,powerOn&&paramValues[p]>0.5,e||0)
else if(t==='gear')drawGear(x,y,false)}
drawKeyboard();if(powerOn){drawScope();drawPresetDisplay()}ctx.restore();
if(menuOpen)drawSettingsMenu()}

// ===== Mouse =====
let dragCtrl=null,dragStartY=0,dragStartVal=0,dragAccum=0,pressedBtn=-1,kbPressedKey=-1,kbDragging=false;
let tuningInSnap=false;
let transposeKey=-1; // keyboard index of current transpose root (-1 = none)
let benderDragStartY=0; // vertical drag start for LFO trigger
const kbTO=[13,27,40,54,66,79,93,103,117,128,142,154],kbBO=[22,22,44,44,66,88,88,110,110,132,132,154];
function m2k(mx,my){const lx=mx-KB.x,ly=my-KB.y-5;if(lx<0||lx>=792||ly<0||ly>=109)return -1;const oc=Math.floor(lx/154),off=lx%154,os=(ly<=56)?kbTO:kbBO;for(let i=0;i<12;i++)if(off<os[i]){const k=oc*12+i;return k<61?k:-1}if(oc===4&&off>=132)return 60;return -1}
function hitC(mx,my){for(let i=controls.length-1;i>=0;i--){const b=ctrlBounds(controls[i]);if(mx>=b.x&&mx<b.x+b.w&&my>=b.y&&my<b.y+b.h)return i}return -1}
function gmp(e){const r=canvas.getBoundingClientRect();return{x:(e.clientX-r.left)*W/r.width,y:(e.clientY-r.top)*H/r.height}}

function initCanvas(){
canvas=document.getElementById('c');canvas.width=W*S;canvas.height=H*S;
canvas.style.width='100%';canvas.style.height='auto';
if(S>=2)canvas.style.imageRendering='pixelated';
ctx=canvas.getContext('2d');
canvas.addEventListener('mousedown',e=>{const pos=gmp(e);
// Preset sheet click
if(sheetOpen){
  if(pos.x>=W-14&&pos.y<14){sheetOpen=false;sheetSearch='';drawAll();e.preventDefault();return}
  const idx=sheetHitTest(pos.x,pos.y);
  if(idx>=0){currentPreset=idx;synth.loadPreset(idx);syncFP(idx)}
  sheetOpen=false;sheetSearch='';drawAll();e.preventDefault();return}
// Settings menu click
if(menuOpen){
  const mw=180,mh=menuItems.length*14;
  const mmx=Math.round((W-mw)/2),mmy=Math.round((H-mh)/2);
  if(pos.x>=mmx&&pos.x<mmx+mw&&pos.y>=mmy&&pos.y<mmy+mh){
    const row=Math.floor((pos.y-mmy)/14);
    if(row>=0&&row<menuItems.length){
      const it=menuItems[row];
      if(it.type==='radio'){menuSettings[it.group]=it.val;applySettings()}
      else if(it.type==='check'){menuSettings[it.key]=!menuSettings[it.key];applySettings()}}}
  else{menuOpen=false}
  drawAll();e.preventDefault();return}
if(pos.x>=790&&pos.x<918&&pos.y>=86&&pos.y<100){togglePP(e);return}
// Scope click
if(pos.x>=SC.x&&pos.x<SC.x+SC.w&&pos.y>=SC.y&&pos.y<SC.y+SC.h){
  const lx=pos.x-SC.x, ly=pos.y-SC.y;
  const ch=SC.h-kNavH;
  // Nav bar click (all modes)
  if(ly>=ch){
    if(lx<kNavArrowW)cycleScopeMode(-1);
    else if(lx>=SC.w-kNavArrowW)cycleScopeMode(1);
    drawAll();e.preventDefault();return}
  // Patch bank mode
  if(scopeMode===4){
    if(pbBallActive){pbBallActive=false;drawAll();e.preventDefault();return}
    const idx=pbPatchAt(lx,ly);
    if(idx>=0){
      currentPreset=idx;synth.loadPreset(idx);syncFP(idx);
      pbDragOriginX=Math.floor(lx/kPBCell)*kPBCell+kPBCell/2;
      pbDragOriginY=Math.floor(ly/kPBCell)*kPBCell+kPBCell/2;
      pbDragEndX=lx;pbDragEndY=ly;pbDragging=true}
    drawAll();e.preventDefault();return}
  // Interactive drags
  scopeDragStartX=e.clientX;scopeDragStartY=e.clientY;
  if(scopeMode===0||scopeMode===1){
    // Waveform/spectrum: drag = master volume
    scopeDragMode=1;scopeDragStartVal[0]=paramValues[P.masterVol]}
  else if(scopeMode===3){
    // VCF: direct position
    scopeDragMode=2;
    paramValues[P.vcfFreq]=Math.max(0,Math.min(1,lx/SC.w));
    paramValues[P.vcfRes]=Math.max(0,Math.min(1,1-ly/ch));
    synth.setParam(P.vcfFreq,paramValues[P.vcfFreq]);
    synth.setParam(P.vcfRes,paramValues[P.vcfRes]);
    if(!liveParams.has(P.vcfFreq))presetDirty=true}
  else if(scopeMode===2){
    // ADSR: drag boundary lines
    const xNorm=lx/SC.w;
    const hitZone=5/SC.w;
    if(Math.abs(xNorm-adsrBoundAD)<hitZone){scopeDragMode=3;adsrDragParam=P.envA}
    else if(Math.abs(xNorm-adsrBoundDS)<hitZone){scopeDragMode=3;adsrDragParam=P.envD}
    else if(Math.abs(xNorm-adsrBoundSR)<hitZone){scopeDragMode=3;adsrDragParam=P.envR}
    else if(xNorm>adsrBoundDS+hitZone&&xNorm<adsrBoundSR-hitZone&&Math.abs(ly/ch-adsrSustainY)<hitZone*3){
      scopeDragMode=3;adsrDragParam=P.envS}
    if(scopeDragMode===3)scopeDragStartVal[0]=paramValues[adsrDragParam]}
  drawAll();e.preventDefault();return}
const key=m2k(pos.x,pos.y);if(key>=0&&paramValues[P.power]>0.5){
  // Transpose mode: click sets transpose offset, doesn't play
  if(paramValues[P.transpose]>0.5){
    const midiNote=key+KB.minNote;
    const offset=midiNote-60; // semitones from middle C
    transposeKey=(offset===0)?-1:key;
    synth.setParam(P.transposeOfs,offset);
    drawAll();return}
  if(paramValues[P.hold]>0.5&&kbKeys[key]){
    kbKeys[key]=0;synth.forceRelease(key+KB.minNote);drawAll();return}
  kbKeys[key]=1;kbPressedKey=key;kbDragging=true;synth.noteOn(key+KB.minNote,127);drawAll();return}
const ci=hitC(pos.x,pos.y);if(ci<0)return;const[p,t]=controls[ci];
if(t==='slider'||t==='knob'){dragCtrl=ci;dragStartY=e.clientY;dragStartVal=paramValues[p];dragAccum=paramValues[p];
  document.body.style.cursor='none'}
else if(t==='switch3'||t==='switch2'){dragCtrl=ci;dragStartY=e.clientY;dragStartVal=Math.round(paramValues[p])}
else if(t==='hswitch3'||t==='hswitch2'){dragCtrl=ci;dragStartY=e.clientX;dragStartVal=Math.round(paramValues[p])}
else if(t==='btnled'){pressedBtn=ci;paramValues[p]=paramValues[p]>0.5?0:1;synth.setParam(p,paramValues[p]);if(!liveParams.has(p))presetDirty=true;
if(p===P.hold&&paramValues[p]<=0.5)kbKeys.fill(0);
drawAll()}
else if(t==='power'){paramValues[p]=paramValues[p]>0.5?0:1;synth.setParam(p,paramValues[p]);if(paramValues[p]<=0.5){kbKeys.fill(0);synth.controlChange(123,0)}drawAll()}
else if(t==='bender'){dragCtrl=ci;dragStartY=e.clientX;dragStartVal=paramValues[p];benderDragStartY=e.clientY;benderTriggered=false}
else if(t==='chorusoff'){pressedBtn=ci;paramValues[P.chorusI]=0;paramValues[P.chorusII]=0;synth.setParam(P.chorusI,0);synth.setParam(P.chorusII,0);if(!liveParams.has(p))presetDirty=true;drawAll()}
else if(t==='gear'){menuOpen=!menuOpen;menuHover=-1;drawAll()}
e.preventDefault()});

// Prevent context menu on scope (right-click cycles mode)
canvas.addEventListener('contextmenu',e=>{const pos=gmp(e);
if(pos.x>=SC.x&&pos.x<SC.x+SC.w&&pos.y>=SC.y&&pos.y<SC.y+SC.h)e.preventDefault()});

const tip=()=>document.getElementById('tooltip');
let tipCtrl=-1;

function showTip(ci){
  const tt=tip();
  if(ci<0){tt.style.display='none';tipCtrl=-1;return}
  const[p,t]=controls[ci];
  const name=t==='chorusoff'?'Chorus Off':paramNames[p]||'';
  if(!name){tt.style.display='none';tipCtrl=-1;return}
  let val='';
  if(t==='slider'||t==='knob')val=paramValueText(p,paramValues[p]);
  else if(t==='switch3'||t==='switch2'||t==='hswitch3'||t==='hswitch2'){
    const labels=switchLabels[p];if(labels)val=labels[Math.round(paramValues[p])]}
  else if(t==='btnled')val=paramValues[p]>0.5?'On':'Off';
  tt.textContent=val?name+': '+val:name;
  tt.style.display='block';
  const b=ctrlBounds(controls[ci]);const rect=canvas.getBoundingClientRect();
  const sx=rect.width/W,sy=rect.height/H;
  tt.style.left=Math.max(0,(b.x+b.w/2)*sx-tt.offsetWidth/2)+'px';
  tt.style.top=((b.y+b.h)*sy+2)+'px';
  tipCtrl=ci;
}

canvas.addEventListener('mouseleave',()=>{tip().style.display='none';tipCtrl=-1});
canvas.addEventListener('mousemove',e=>{
if(sheetOpen){const pos=gmp(e);
  const overClose=pos.x>=W-14&&pos.y<14;
  const idx=overClose?-2:sheetHitTest(pos.x,pos.y);
  if(idx!==sheetHover){sheetHover=idx;drawPresetSheet()}tip().style.display='none';tipCtrl=-1;return}
if(menuOpen){const pos=gmp(e);
  const mw=180,mh=menuItems.length*14;
  const mmx=Math.round((W-mw)/2),mmy=Math.round((H-mh)/2);
  let row=-1;
  if(pos.x>=mmx&&pos.x<mmx+mw&&pos.y>=mmy&&pos.y<mmy+mh)
    row=Math.floor((pos.y-mmy)/14);
  if(row>=0&&row<menuItems.length&&(menuItems[row].type==='sep'||menuItems[row].type==='label'))row=-1;
  if(row!==menuHover){menuHover=row;drawSettingsMenu()}
  tip().style.display='none';tipCtrl=-1;return}
if(dragCtrl!==null){showTip(dragCtrl);return}
const pos=gmp(e);
// Scope nav hover (all modes)
if(pos.x>=SC.x&&pos.x<SC.x+SC.w&&pos.y>=SC.y&&pos.y<SC.y+SC.h){
  const ly=pos.y-SC.y,lx=pos.x-SC.x;
  const ch=SC.h-kNavH;
  let nh=-1;
  if(ly>=ch) nh=lx<kNavArrowW?0:(lx>=SC.w-kNavArrowW?1:-1);
  if(nh!==navHover){navHover=nh;drawAll()}
  // ADSR cursor
  if(scopeMode===2&&ly<ch){
    const xNorm=lx/SC.w,hitZone=5/SC.w;
    if(Math.abs(xNorm-adsrBoundAD)<hitZone||Math.abs(xNorm-adsrBoundDS)<hitZone||Math.abs(xNorm-adsrBoundSR)<hitZone)
      canvas.style.cursor='ew-resize';
    else if(xNorm>adsrBoundDS+hitZone&&xNorm<adsrBoundSR-hitZone&&Math.abs(ly/ch-adsrSustainY)<hitZone*3)
      canvas.style.cursor='ns-resize';
    else canvas.style.cursor=''}}
else{if(navHover>=0){navHover=-1;drawAll()}canvas.style.cursor=''}
const ci=hitC(pos.x,pos.y);
if(ci!==tipCtrl)showTip(ci)});

window.addEventListener('mousemove',e=>{
// Scope interactive drags
if(scopeDragMode>0){
  const dx=e.clientX-scopeDragStartX,dy=e.clientY-scopeDragStartY;
  const gear=e.shiftKey?500:128;
  const ch=SC.h-kNavH;
  if(scopeDragMode===1){
    // Volume
    paramValues[P.masterVol]=Math.max(0,Math.min(1,scopeDragStartVal[0]+(dx-dy)/gear));
    synth.setParam(P.masterVol,paramValues[P.masterVol])}
  else if(scopeDragMode===2){
    // VCF: direct position
    const pos=gmp(e);
    const lx=pos.x-SC.x,ly=pos.y-SC.y;
    paramValues[P.vcfFreq]=Math.max(0,Math.min(1,lx/SC.w));
    paramValues[P.vcfRes]=Math.max(0,Math.min(1,1-ly/ch));
    synth.setParam(P.vcfFreq,paramValues[P.vcfFreq]);
    synth.setParam(P.vcfRes,paramValues[P.vcfRes]);
    if(!liveParams.has(P.vcfFreq))presetDirty=true}
  else if(scopeDragMode===3){
    // ADSR
    let delta;
    if(adsrDragParam===P.envS)delta=-dy/gear;
    else if(adsrDragParam===P.envR)delta=-dx/gear;
    else delta=dx/gear;
    paramValues[adsrDragParam]=Math.max(0,Math.min(1,scopeDragStartVal[0]+delta));
    synth.setParam(adsrDragParam,paramValues[adsrDragParam]);
    if(!liveParams.has(adsrDragParam))presetDirty=true}
  drawAll();return}
if(pbDragging){const pos=gmp(e);
  pbDragEndX=Math.max(0,Math.min(SC.w-1,pos.x-SC.x));
  pbDragEndY=Math.max(0,Math.min(SC.h-1,pos.y-SC.y));
  drawAll();return}
if(kbDragging){const pos=gmp(e);const key=m2k(pos.x,pos.y);
if(key!==kbPressedKey){
  if(kbPressedKey>=0){if(paramValues[P.hold]<=0.5)kbKeys[kbPressedKey]=0;synth.noteOff(kbPressedKey+KB.minNote)}
  if(key>=0){kbKeys[key]=1;kbPressedKey=key;synth.noteOn(key+KB.minNote,127)}else{kbPressedKey=-1}
  drawAll()}return}
if(dragCtrl===null)return;const[p,t]=controls[dragCtrl];
if(t==='slider'||t==='knob'){
  const dy=dragStartY-e.clientY;dragStartY=e.clientY;
  let gear=p===P.tuning?205:127;
  if(e.metaKey||e.ctrlKey)gear*=100;else if(e.shiftKey)gear*=10;
  dragAccum=Math.max(0,Math.min(1,dragAccum+dy/gear));
  let nv=dragAccum;
  // Tuning: three-zone center snap (100px neg, 6px detent, 100px pos)
  if(p===P.tuning){
    const px=nv*205,half=102.5,snapHalf=3;
    const negEnd=half-snapHalf,posStart=half+snapHalf;
    if(px<=negEnd)nv=(px/negEnd)*0.495;
    else if(px>=posStart)nv=0.505+((px-posStart)/(205-posStart))*0.495;
    else{nv=0.5;if(!tuningInSnap)dragAccum=0.5;tuningInSnap=true}
    if(px<=negEnd||px>=posStart)tuningInSnap=false}
  if(p===P.hpfFreq)nv=Math.round(nv*3)/3;
  paramValues[p]=nv;synth.setParam(p,p===P.hpfFreq?nv*3:p===P.tuning?nv*2-1:nv);if(!liveParams.has(p))presetDirty=true}
else if(t==='switch3'||t==='switch2'){const mx=t==='switch3'?2:1;paramValues[p]=Math.max(0,Math.min(mx,Math.round(dragStartVal+(e.clientY-dragStartY)/12)));synth.setParam(p,paramValues[p]);if(!liveParams.has(p))presetDirty=true}
else if(t==='hswitch3'||t==='hswitch2'){const mx=t==='hswitch3'?2:1;
  const oldVal=paramValues[p];
  paramValues[p]=Math.max(0,Math.min(mx,Math.round(dragStartVal+(e.clientX-dragStartY)/12)));synth.setParam(p,paramValues[p]);if(!liveParams.has(p))presetDirty=true;
  if(p===P.adsrMode&&Math.round(oldVal)!==Math.round(paramValues[p])){
    // DSP remap already happened inside setParam; read back remapped slider values
    paramValues[P.vcfFreq]=synth.getVcfSlider();
    paramValues[P.hpfFreq]=synth.getHpfSlider()/3;
    synth.setBankOffset(paramValues[p]<0.5?0:1)}}
else if(t==='bender'){const dx=e.clientX-dragStartY;paramValues[p]=Math.max(0,Math.min(1,dragStartVal+dx/100));synth.setParam(p,paramValues[p]*2-1);
  const trig=e.clientY<benderDragStartY-8;if(trig!==benderTriggered){benderTriggered=trig;synth.controlChange(1,trig?1:0)}}
showTip(dragCtrl);drawAll()});

window.addEventListener('mouseup',()=>{
if(scopeDragMode>0){scopeDragMode=0;canvas.style.cursor='';drawAll()}
if(pbDragging){
  pbDragging=false;
  const dx=pbDragEndX-pbDragOriginX,dy=pbDragEndY-pbDragOriginY;
  const mag=Math.sqrt(dx*dx+dy*dy);
  const releaseIdx=pbPatchAt(pbDragEndX,pbDragEndY);
  const originIdx=pbPatchAt(pbDragOriginX,pbDragOriginY);
  if(releaseIdx!==originIdx&&mag>1){
    const norm=Math.min(mag/80,1);
    pbBallSpeed=norm*norm*0.45;pbBallAccum=0;pbBallErrX=0;pbBallErrY=0;
    pbBallCellX=originIdx%kPBCols;pbBallCellY=Math.floor(originIdx/kPBCols);
    pbBallDX=-dx/mag;pbBallDY=-dy/mag;pbBallActive=true}
  drawAll()}
if(dragCtrl!==null){
  const dt=controls[dragCtrl][1];
  if(dt==='bender'){paramValues[P.bender]=0.5;synth.setParam(P.bender,0);if(benderTriggered){benderTriggered=false;synth.controlChange(1,0)}drawAll()}
  if(dt==='slider'||dt==='knob'){document.body.style.cursor='';
    const dp=controls[dragCtrl][0];
    if(dp===P.hpfFreq){const sn=Math.round(paramValues[dp]*3)/3;if(sn!==paramValues[dp]){paramValues[dp]=sn;synth.setParam(dp,sn*3)}}}}
dragCtrl=null;tip().style.display='none';tipCtrl=-1;if(pressedBtn>=0){pressedBtn=-1;drawAll()}
if(kbDragging){if(kbPressedKey>=0){if(paramValues[P.hold]<=0.5)kbKeys[kbPressedKey]=0;synth.noteOff(kbPressedKey+KB.minNote)}kbPressedKey=-1;kbDragging=false;drawAll()}});}

// ===== QWERTY =====
let QBASE=48;
function qn(k){switch(k){case'z':return QBASE;case'x':return QBASE+2;case'c':return QBASE+4;case'v':return QBASE+5;case'b':return QBASE+7;case'n':return QBASE+9;case'm':return QBASE+11;case',':return QBASE+12;case'.':return QBASE+14;case'/':return QBASE+16;case's':return QBASE+1;case'd':return QBASE+3;case'g':return QBASE+6;case'h':return QBASE+8;case'j':return QBASE+10;case'l':return QBASE+13;case';':return QBASE+15;case"'":return QBASE+17;case'q':return QBASE+12;case'w':return QBASE+14;case'e':return QBASE+16;case'r':return QBASE+17;case't':return QBASE+19;case'y':return QBASE+21;case'u':return QBASE+23;case'i':return QBASE+24;case'o':return QBASE+26;case'p':return QBASE+28;case'[':return QBASE+29;case']':return QBASE+31;case'2':return QBASE+13;case'3':return QBASE+15;case'5':return QBASE+18;case'6':return QBASE+20;case'7':return QBASE+22;case'9':return QBASE+25;case'0':return QBASE+27;case'-':return QBASE+30;case'=':return QBASE+32;default:return -1}}
const qd={};
document.addEventListener('keydown',e=>{
if(!audioStarted)return;
if(sheetOpen){
  if(e.key==='Escape'){
    if(sheetSearch){sheetSearch='';drawPresetSheet()}
    else{sheetOpen=false;sheetSearch='';drawAll()}
    e.preventDefault();return}
  if(e.key==='Backspace'){
    if(sheetSearch){sheetSearch=sheetSearch.slice(0,-1);drawPresetSheet()}
    e.preventDefault();return}
  if(e.key==='Enter'){
    if(sheetHover>=0){currentPreset=sheetHover;synth.loadPreset(sheetHover);syncFP(sheetHover)}
    sheetOpen=false;sheetSearch='';drawAll();e.preventDefault();return}
  if(e.key==='ArrowRight'){
    const n=sheetSearch?sheetFindNext(sheetHover+1,1):((sheetHover<0?currentPreset:sheetHover)+1)%128;
    if(n>=0)sheetHover=n;drawPresetSheet();e.preventDefault();return}
  if(e.key==='ArrowLeft'){
    const n=sheetSearch?sheetFindNext(sheetHover-1,-1):((sheetHover<0?currentPreset:sheetHover)+127)%128;
    if(n>=0)sheetHover=n;drawPresetSheet();e.preventDefault();return}
  if(e.key==='ArrowDown'){
    let start=sheetHover<0?currentPreset:sheetHover;
    const col=start%sheetCols;
    for(let i=1;i<=sheetRows;i++){const idx=((start+sheetCols*i)%128+128)%128;
      if((idx%sheetCols)===col&&(!sheetSearch||sheetMatches(idx))){sheetHover=idx;break}}
    drawPresetSheet();e.preventDefault();return}
  if(e.key==='ArrowUp'){
    let start=sheetHover<0?currentPreset:sheetHover;
    const col=start%sheetCols;
    for(let i=1;i<=sheetRows;i++){const idx=((start-sheetCols*i)%128+128)%128;
      if((idx%sheetCols)===col&&(!sheetSearch||sheetMatches(idx))){sheetHover=idx;break}}
    drawPresetSheet();e.preventDefault();return}
  if(e.key.length===1&&e.key>=' '&&e.key.charCodeAt(0)<127){
    sheetSearch+=e.key;
    if(sheetSearch)sheetHover=sheetFindNext(0,1);
    drawPresetSheet();e.preventDefault();return}
  return}
if(menuOpen){if(e.key==='Escape'){menuOpen=false;drawAll()}return}
if(e.key==='ArrowUp'||e.key==='ArrowDown'){
  const dir=e.key==='ArrowUp'?-1:1;const n=synth.getNumPresets();if(n>0){currentPreset=((currentPreset+dir)%n+n)%n;synth.loadPreset(currentPreset);syncFP(currentPreset);drawAll()}e.preventDefault();return}
if(e.key==='ArrowLeft'||e.key==='ArrowRight'){
  cycleScopeMode(e.key==='ArrowRight'?1:-1);drawAll();e.preventDefault();return}
if(e.key==='PageUp'||e.key==='PageDown'){
  const dir=e.key==='PageUp'?-8:8;const n=synth.getNumPresets();if(n>0){currentPreset=((currentPreset+dir)%n+n)%n;synth.loadPreset(currentPreset);syncFP(currentPreset);drawAll()}e.preventDefault();return}
if(e.key==='Enter'){togglePP(e);e.preventDefault();return}
if(e.key==='`'){QBASE=Math.max(24,QBASE-12);e.preventDefault();return}
if(e.key==='1'){QBASE=Math.min(84,QBASE+12);e.preventDefault();return}
if(e.repeat||!canvas||paramValues[P.power]<=0.5)return;const n=qn(e.key.toLowerCase());if(n<0)return;const ki=n-KB.minNote;if(ki<0||ki>=61||qd[n])return;
if(paramValues[P.transpose]>0.5){
  const offset=n-60;transposeKey=(offset===0)?-1:ki;
  synth.setParam(P.transposeOfs,offset);drawAll();return}
qd[n]=1;kbKeys[ki]=1;synth.noteOn(n,127);drawAll()});
document.addEventListener('keyup',e=>{if(!canvas)return;const n=qn(e.key.toLowerCase());if(n<0||!qd[n])return;delete qd[n];const ki=n-KB.minNote;if(ki>=0&&ki<61)kbKeys[ki]=0;synth.noteOff(n);drawAll()});

// Release all notes on blur/tab away
function releaseAll(){
  pbBallActive=false;pbDragging=false;
  // Reset bender if it was being dragged
  if(dragCtrl!==null&&controls[dragCtrl][1]==='bender'){
    paramValues[P.bender]=0.5;synth.setParam(P.bender,0)}
  if(dragCtrl!==null&&(controls[dragCtrl][1]==='slider'||controls[dragCtrl][1]==='knob'))
    document.body.style.cursor='';
  dragCtrl=null;kbDragging=false;
  for(const n in qd){const note=parseInt(n);synth.noteOff(note);const ki=note-KB.minNote;if(ki>=0&&ki<61)kbKeys[ki]=0}
  for(const k in qd)delete qd[k];
  if(kbPressedKey>=0){if(paramValues[P.hold]<=0.5)kbKeys[kbPressedKey]=0;synth.noteOff(kbPressedKey+KB.minNote);kbPressedKey=-1}
  if(synth.ctx&&synth.ctx.state==='running')synth.ctx.suspend();
  drawAll()}
function resumeAudio(){
  if(synth.ctx&&synth.ctx.state==='suspended')synth.ctx.resume()}
window.addEventListener('blur',releaseAll);
window.addEventListener('focus',resumeAudio);
document.addEventListener('visibilitychange',()=>{if(document.hidden)releaseAll();else resumeAudio()});

// ===== Preset Sheet =====
let sheetOpen=false, sheetHover=-1, sheetSearch='';
const sheetCols=8, sheetRows=16;

function sheetHitTest(mx,my){
  const colW=W/sheetCols, rowH=H/sheetRows;
  const col=Math.floor(mx/colW), row=Math.floor(my/rowH);
  if(col<0||col>=sheetCols||row<0||row>=sheetRows)return -1;
  const idx=row*sheetCols+col;
  return idx<128?idx:-1;
}
function sheetMatches(idx){
  if(!sheetSearch)return true;
  return synth.getPresetName(idx).toLowerCase().includes(sheetSearch.toLowerCase());
}
function sheetFindNext(from,step){
  for(let i=0;i<128;i++){const idx=((from+step*i)%128+128)%128;if(sheetMatches(idx))return idx}
  return -1;
}

function drawPresetSheet(){
  ctx.save();ctx.scale(S,S);
  ctx.fillStyle='#000';ctx.fillRect(0,0,W,H);
  const colW=W/sheetCols, rowH=H/sheetRows;
  ctx.font="11px 'Segment14', monospace";

  ctx.fillStyle='#002800';
  for(let row=0;row<=sheetRows;row++){const y=Math.round(row*rowH);ctx.fillRect(0,y,W,1)}
  for(let col=1;col<sheetCols;col++){const x=Math.round(col*colW);ctx.fillRect(x,0,1,H)}

  for(let i=0;i<128;i++){
    const col=i%sheetCols, row=Math.floor(i/sheetCols);
    const x=Math.round(col*colW), y=Math.round(row*rowH);
    const cw=Math.round((col+1)*colW)-x, ch=Math.round((row+1)*rowH)-y;
    const name=synth.getPresetName(i).substring(0,16);
    const matches=sheetMatches(i);

    if(i===currentPreset){
      ctx.fillStyle='#00ff00';ctx.fillRect(x,y,cw,ch);
      ctx.fillStyle='#000';
    }else if(matches&&i===sheetHover){
      ctx.fillStyle='#003c00';ctx.fillRect(x,y,cw,ch);
      ctx.fillStyle='#00ff00';
    }else{
      ctx.fillStyle=matches?'#00ff00':'#005a00';
    }
    ctx.fillText(name,x+4,y+ch-2);
  }

  const bx=W-14;
  ctx.fillStyle=sheetHover===-2?'#003c00':'#000';ctx.fillRect(bx,0,14,14);
  ctx.strokeStyle='#006400';ctx.lineWidth=1;ctx.strokeRect(bx+0.5,0.5,13,13);
  ctx.fillStyle='#00ff00';
  ctx.font="13px 'Segment14', monospace";ctx.fillText('x',bx+3,12);

  ctx.strokeStyle='#006400';ctx.lineWidth=1;ctx.strokeRect(0,0,W,H);
  ctx.restore();
}

function togglePP(e){
  menuOpen=false;
  sheetOpen=!sheetOpen;
  if(sheetOpen){sheetHover=-1;sheetSearch='';drawPresetSheet()}else{sheetSearch='';drawAll()}}

// ===== Preset sync =====
// All live performance params excluded from presets (matches native PluginProcessor.cpp)
const liveParams=new Set([
  P.benderDco,P.benderVcf,P.arpRate, // sliders 0-19
  P.transpose,P.hold,P.arpeggio,P.arpMode,P.arpRange, // switches 20-39
  P.bender,P.tuning,P.power,P.portaMode, // switches 20-39
  P.portaRate, // slider 40
  P.transposeOfs,P.benderLfo, // switches 41-43
  P.masterVol // 44
]);
function syncFP(pi){
  for(let i=0;i<=19;i++){if(liveParams.has(i))continue;if(i===P.hpfFreq){paramValues[i]=synth.getPresetValue(pi,i)/3;continue}paramValues[i]=synth.getPresetValue(pi,i)/127}
  if(!liveParams.has(P.portaRate))paramValues[P.portaRate]=synth.getPresetValue(pi,40)/127;
  for(let i=20;i<=39;i++){if(liveParams.has(i))continue;paramValues[i]=synth.getPresetValue(pi,i)}
  for(let i=41;i<=43;i++){if(liveParams.has(i))continue;paramValues[i]=synth.getPresetValue(pi,i)}
  presetDirty=false;
}

// ===== Settings =====
function loadSettings(){
  try{const s=localStorage.getItem('kr106_settings');if(s)Object.assign(menuSettings,JSON.parse(s))}catch(e){}}
function saveSettings(){
  try{localStorage.setItem('kr106_settings',JSON.stringify(menuSettings))}catch(e){}}
function applySettings(){
  synth.setVoices(menuSettings.voices);
  synth.setIgnoreVelocity(menuSettings.ignoreVel);
  if(menuSettings.midiEnabled&&!synth._midiInit){
    synth.initMidi().then(()=>{synth._midiInit=true;if(menuOpen)drawSettingsMenu()})
      .catch((e)=>{menuSettings.midiEnabled=false;console.warn('MIDI init failed:',e);if(menuOpen)drawSettingsMenu()})}
  saveSettings();
  if(menuOpen)drawSettingsMenu()}

// ===== Startup =====
let audioStarted=false;
async function startAudio(){
if(audioStarted)return;audioStarted=true;
document.getElementById('splash').style.display='none';
document.getElementById('synth-wrap').style.display='block';
initCanvas();
paramValues[P.power]=1;paramValues[P.tuning]=0.5;paramValues[P.masterVol]=0.5;paramValues[P.bender]=0.5;
try{
  await loadImages();
  await document.fonts.load("11px 'Segment14'");
  drawAll();
  await synth.init();
  currentPreset=Math.floor(Math.random()*synth.getNumPresets());
  synth.loadPreset(currentPreset);syncFP(currentPreset);
  paramValues[P.tuning]=0.5;paramValues[P.masterVol]=0.5;paramValues[P.bender]=0.5;
  paramValues[P.power]=1;paramValues[P.arpRate]=0.35;paramValues[P.portaMode]=1;
  loadSettings();applySettings();
  drawAll();
  synth.onMidiNote=(note,on)=>{
    if(on&&paramValues[P.transpose]>0.5){
      const offset=note-60;
      const ki=note-KB.minNote;
      transposeKey=(offset===0)?-1:(ki>=0&&ki<61?ki:-1);
      synth.setParam(P.transposeOfs,offset);
      drawAll();return}
    const ki=note-KB.minNote;
    if(ki>=0&&ki<61){
      if(on)kbKeys[ki]=1;
      else if(paramValues[P.hold]<=0.5)kbKeys[ki]=0;
      drawAll()}};
  // AudioWorklet: VCF/HPF slider readback arrives async after preset load or model switch
  synth.onPresetValues=(data)=>{
    if(data.vcfSlider!==undefined)paramValues[P.vcfFreq]=data.vcfSlider;
    if(data.hpfSlider!==undefined)paramValues[P.hpfFreq]=data.hpfSlider/3;
    drawAll();
  };
  synth.onSliderUpdate=(data)=>{
    if(data.vcfSlider!==undefined)paramValues[P.vcfFreq]=data.vcfSlider;
    if(data.hpfSlider!==undefined)paramValues[P.hpfFreq]=data.hpfSlider/3;
    drawAll();
  };
  // MIDI disabled by default — will be enabled from settings menu
  // try{await synth.initMidi()}catch(e){}
  let lastFrame=0;function scopeLoop(t){requestAnimationFrame(scopeLoop);if(t-lastFrame<66)return;lastFrame=t;
if(!sheetOpen&&paramValues[P.power]>0.5){if(scopeClipHold>0)scopeClipHold--;ctx.save();ctx.scale(S,S);drawScope();drawClipLED(53,127);ctx.restore()}}scopeLoop(0);
}catch(e){console.error(e)}}
