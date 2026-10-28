const fs=require('fs'), vm=require('vm'), assert=require('assert/strict');
const source=fs.readFileSync(require('path').join(__dirname,'../../assets/apps/audio-test/playback.html'),'utf8').match(/<script>([\s\S]*?)<\/script>/)[1];
const elements={play:{},stop:{},status:{}};
let closed=0,resumed=0,started=0;
class Context {
 constructor(){this.sampleRate=48000;this.destination={};}
 async resume(){resumed++;}
 async close(){closed++;}
 createOscillator(){return {frequency:{},connect(target){return target;},start(){started++;}};}
 createGain(){return {gain:{},connect(target){return target;}};}
}
const browserStop=function(){};
const globals={document:{getElementById:id=>elements[id]},AudioContext:Context,
 play:elements.play,stop:browserStop};
vm.runInNewContext(source,globals);
(async()=>{
 assert.equal(typeof elements.play.onclick,'function');
 assert.equal(typeof elements.stop.onclick,'function','停止必须绑定实际按钮');
 assert.equal(browserStop.onclick,undefined,'不能改写浏览器 window.stop');
 for(let i=0;i<4;i++){
  await elements.play.onclick();assert.equal(elements.status.textContent,'正在播放 · 48000 Hz');
  await elements.stop.onclick();assert.equal(elements.status.textContent,'已停止');
  await elements.stop.onclick();assert.equal(closed,i+1);
 }
 assert.equal(resumed,4);assert.equal(started,4);
 console.log('Audio playback UI: browser stop name collision, four start/stop cycles and repeated stop passed');
})().catch(e=>{console.error(e);process.exitCode=1;});
