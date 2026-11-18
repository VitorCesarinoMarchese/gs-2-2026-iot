const $=id=>document.getElementById(id), T=THREE, stage=$('stage');
const renderer=new T.WebGLRenderer({alpha:true,antialias:true});
renderer.setPixelRatio(Math.min(devicePixelRatio,2));stage.prepend(renderer.domElement);
const scene=new T.Scene(),camera=new T.OrthographicCamera(-4,4,4,-4,.1,100);
camera.position.z=14;
scene.add(new T.HemisphereLight(0xfffdf1,0x627058,2.3));
const light=new T.DirectionalLight(0xffffff,3);light.position.set(-3,6,7);scene.add(light);
function group(parent){const g=new T.Group();parent.add(g);return g}
const body=group(scene),lid=group(body),board=group(body),slider=group(lid);
function mat(color,metalness=.15){return new T.MeshStandardMaterial({color,metalness,roughness:.47})}
const dark=mat(0x252b26),silver=mat(0xbac1b5,.65), face=mat(0xd9d1bf),accent=mat(0xf26b35);
function mesh(parent,geo,m,x=0,y=0,z=0){const o=new T.Mesh(geo,m);parent.add(o);o.position.set(x,y,z);return o}
function circle(r,x=0,y=0){const s=new T.Shape();s.absarc(x,y,r,0,Math.PI*2);return s}
function shell(parent,r,depth,m,z,holes=[]){const s=circle(r);for(const [x,y,a] of holes)s.holes.push(circle(a,x,y));return mesh(parent,new T.ExtrudeGeometry(s,{depth,bevelThickness:.055,bevelSize:.055,bevelSegments:4,curveSegments:80}),m,0,0,z)}
shell(body,2.4,.13,dark,-1.02);shell(body,2.4,.94,dark,-.85,[[0,0,2.16]]);
function ring(p,r,t,m,x,y,z){return mesh(p,new T.TorusGeometry(r,t,16,96),m,x,y,z)}
ring(body,.38,.14,dark,0,2.48,-.18);ring(body,.63,.072,silver,.12,3.15,-.19);
function box(p,w,h,d,m,x,y,z){
  return mesh(p,new T.BoxGeometry(w,h,d),m,x,y,z);
}
function disk(p,r,h,m,x,y,z){
  const part=mesh(p,new T.CylinderGeometry(r,r,h,80),m,x,y,z);
  part.rotation.x=Math.PI/2;
  return part;
}
function print(p,value,x,y,z,w,h,color='#f0ede3'){
  const c=document.createElement('canvas');c.width=640;c.height=100;
  const ctx=c.getContext('2d');ctx.font='bold 74px monospace';ctx.fillStyle=color;
  ctx.textAlign='center';ctx.textBaseline='middle';ctx.fillText(value,320,54);
  const texture=new T.CanvasTexture(c);texture.colorSpace=T.SRGBColorSpace;
  mesh(p,new T.PlaneGeometry(w,h),new T.MeshBasicMaterial({map:texture,transparent:true,depthWrite:false}),x,y,z);
}
const graphite=mat(0x282b2b),gold=mat(0xc3a15e,.55);
shell(lid,2.34,.16,face,.11,[[0,.12,1.48],[-1.94,0,.14],[1.94,0,.14],[0,1.88,.11]]);
ring(lid,1.49,.09,graphite,0,.12,.31);
const button=group(lid);button.position.set(0,.12,.38);
disk(button,1.40,.20,accent,0,0,0);
for(const r of [1.28,1.23])ring(button,r,.014,mat(0xb84e2c),0,0,.106);
print(button,'SOS',0,.01,.105,1.47,.40,'#502b21');
disk(lid,.105,.06,mat(0x95bcac),0,1.88,.31);
for(const x of [-1.94,1.94]){
 disk(lid,.15,.055,silver,x,0,.315);
 for(const a of [0,Math.PI/2])box(lid,.17,.034,.011,graphite,x,0,.345).rotation.z=a+.55;
 disk(body,.22,.29,graphite,x,0,-.16);
}
const track=box(lid,1.28,.42,.08,graphite,0,-1.73,.35);
slider.position.set(-.32,track.position.y,.39);
const thumb=box(slider,.48,.29,.10,silver,0,0,.05);
for(const x of [-.10,0,.10])box(slider,.024,.16,.014,graphite,x,0,.106);
// A stepped tongue offsets the contact point so straight travel clears the circular cap.
box(slider,.38,.10,.05,silver,.15,.07,-.137);
box(slider,.08,.46,.05,silver,.32,.29,-.137);
print(lid,'LOCK   /   OPEN',0,-2.08,.339,1.12,.12,'#272d2c');
const battery=group(body);
const foil=mat(0xbac3ca,.60),tape=mat(0xd9a233);
box(battery,1.78,2.12,.36,foil,0,0,0);
box(battery,1.8,.24,.37,tape,0,1,0);
box(battery,1.36,1.08,.012,mat(0x354445),0,0,.184);
print(battery,'LiPo',0,.15,.192,1.55,.31);
print(battery,'3.7 V',0,-.17,.192,1.4,.20);
for(const [x,color] of [[-.09,0xbf4032],[.09,0x27292a]]){
 const wire=new T.CatmullRomCurve3([new T.Vector3(x,1.10,.05),new T.Vector3(x+.1,1.34,.07),new T.Vector3(x+.46,1.40,.17)]);
 mesh(battery,new T.TubeGeometry(wire,18,.027,8,false),mat(color));
}
box(battery,.30,.20,.15,mat(0xece7d9),.46,1.48,.17);
battery.position.z=-.54;
for(let i=0;i<72;i++){
 const a=i*2*Math.PI/72;
 box(body,.045,.10,.28,graphite,2.42*Math.cos(a),2.42*Math.sin(a),-.16).rotation.z=a;
}
const pcb=mat(0x245544),solder=mat(0xb5bcb3,.52);
disk(board,1.96,.075,pcb,0,0,-.16);
function trace(points,color){
 const path=points.map(([x,y])=>new T.Vector3(x,y,-.115));
 for(let i=1;i<path.length;i++){
  const d=path[i].clone().sub(path[i-1]),o=mesh(board,new T.CylinderGeometry(.015,.015,d.length(),8),color);
  o.position.copy(path[i-1]).addScaledVector(d,.5);o.quaternion.setFromUnitVectors(new T.Vector3(0,1,0),d.normalize());
 }
}
const parts=[
 ['esp','ESP32 DevKitC V4','Controller with Wi-Fi, antenna, USB and two pin headers. TX and RX connect to the Wokwi serial monitor.',-.7,.11,.65],
 ['btn','Panic switch','GPIO 4 uses the internal pull-up. Pressing the switch connects the input to ground.',0,.12,.29],
 ['led','RGB status LED','Common cathode to ground. Red uses GPIO 27, green GPIO 26 and blue GPIO 25.',0,1.56,.24],
 ['res','LED resistors','Three 220 Ω resistors limit LED current. R1 is blue, R2 green and R3 red.',.66,.90,.42],
 ['bz','Piezo buzzer','GPIO 23 drives the buzzer. Its other terminal connects to ground.',1.21,-.18,.40],
 ['pot','Battery simulator','Wokwi potentiometer between 3V3 and ground. The wiper feeds GPIO 34 to simulate battery voltage.',.52,-1.22,.37]
];
const logic=group(board);logic.position.set(-.87,.12,0);
box(logic,.92,2.05,.08,graphite,0,0,-.04);
box(logic,.61,.81,.17,silver,0,.14,.075);
print(logic,'ESP32',0,.14,.163,.52,.15,'#384040');
for(let i=0;i<19;i++)for(const x of [-.397,.397]){
 box(logic,.084,.076,.065,gold,x,-.91+i*.10,.04);
}
for(let i=0;i<5;i++){
 box(logic,.071,.20,.016,gold,-.22+i*.11,.79,.013);
 box(logic,.11,.026,.016,gold,-.165+i*.11,.79+(i%2?.09:-.09),.014);
}
box(logic,.43,.30,.17,silver,0,-.94,.06);
box(logic,.29,.09,.035,dark,0,-1.055,.145);
box(board,.43,.44,.10,solder,0,.12,-.04);
disk(board,.155,.14,dark,0,.12,.064);
disk(board,.17,.20,mat(0xb2d7c4),0,1.56,.012);
for(let i=0;i<3;i++){
 const x=.39+i*.27, channel=[0x506fba,0x4b9562,0xaf5848][i],m=mat(channel);
 box(board,.115,.31,.07,mat(0xdab781),x,.90,-.04);
 for(const y of [.79,1.01])box(board,.126,.064,.08,solder,x,y,-.035);
 for(const y of [.84,.90,.96])box(board,.116,.024,.01,dark,x,y,.001);
 trace([[-.47,.32+i*.12],[x,.32+i*.12],[x,.78]],m);
 trace([[x,1.04],[x,1.25+i*.07],[.09-i*.08,1.5]],m);
}
disk(board,.34,.26,graphite,1.21,-.18,.033);
disk(board,.095,.012,dark,1.21,-.18,.17);
box(board,.55,.50,.19,mat(0x447fa7),.52,-1.22,-.006);
disk(board,.183,.12,silver,.52,-1.22,.13);
box(board,.23,.04,.015,dark,.52,-1.22,.198).rotation.z=.6;
for(const [name,x,y] of [['SW1',.03,-.25],['BZ1',1.2,-.65],['ADC',.51,-1.59],['RGB',-.35,1.54],['220R ×3',.78,1.16]])
 print(board,name,x,y,.002,.44,.11);
const copper=mat(0xb99c66);
trace([[-.47,.1],[-.19,.1]],copper);
trace([[-.47,-.18],[-.15,-.47],[.72,-.47],[1.21,-.18]],copper);
trace([[-.47,-.65],[-.25,-.89],[.5,-.89],[.5,-1.1]],copper);
const highlight=ring(board,.5,.024,accent,0,0,.22);
function inspect(){
 const [, ,note,x,y,r]=parts[$('pins').selectedIndex];
 highlight.position.set(x,y,.25);highlight.scale.setScalar(r/.5);$('details').textContent=note;
}
for(const [id,name] of parts)$('pins').add(new Option(name,id));
$('pins').onchange=inspect;inspect();
let mode='exterior',locked=true,pressed=0;
function reset(){
 body.rotation.set(...(mode==='batteryView'?[-.14,-.12,0]:mode==='circuit'?[0,0,0]:mode==='explode'?[-.85,-.33,-.12]:[-.43,-.30,-.26]));
}
function update(){
 const outside=mode==='exterior';
 $('controls').hidden=!outside;$('inspection').hidden=mode!=='circuit';
 $('press').disabled=locked||!outside;lid.visible=outside||mode==='explode';board.visible=mode!=='batteryView';highlight.visible=mode==='circuit';
 for(const id of ['exterior','circuit','batteryView','explode'])$(id).setAttribute('aria-pressed',mode===id);
 $('guard').setAttribute('aria-pressed',locked);$('guard').textContent=locked?'Unlock slider':'Lock slider';
 $('viewTitle').textContent={exterior:'01 / EXTERIOR',circuit:'02 / CIRCUIT',batteryView:'03 / BATTERY BAY',explode:'04 / EXPLODED ASSEMBLY'}[mode];
 $('assembly').textContent={exterior:'Two screws release the faceplate for service.',circuit:'Wokwi components in a compact illustrative layout. Select a part to locate it and read its connections.',batteryView:'A removable single-cell 3.7 V LiPo sits beneath the circuit carrier. Lift the board and unplug the two-pin lead to replace the pack. Wokwi simulates the voltage with its potentiometer.',explode:'Rear housing, removable LiPo, circuit carrier and faceplate, in assembly order.'}[mode];
}
for(const id of ['exterior','circuit','batteryView','explode'])$(id).onclick=()=>{mode=id;reset();update()};
$('guard').onclick=()=>{locked=!locked;$('feedback').textContent=locked?'The slider blocks travel beneath the button rim.':'Slider released. Press the cap to test it.';update()};
function testPress(){if(locked||mode!=='exterior')return;pressed=performance.now()+450;$('feedback').textContent='Simulated press registered on GPIO 4.'}
$('press').onclick=testPress;
for(const [name,color] of Object.entries({Ceramic:'#d9d1bf',Clay:'#d48e71',Glacier:'#98c0d5',Ink:'#555668'})){const b=document.createElement('button');b.className='sw';b.title=name;b.ariaLabel=name;b.style.background=color;b.setAttribute('aria-pressed',name==='Ceramic');$('colors').append(b);b.onclick=()=>{face.map?.dispose();face.map=null;face.color.set(color);face.needsUpdate=true;$('skin').value='';$('finish').textContent=name;for(const sw of $('colors').children)sw.setAttribute('aria-pressed',sw===b)}}
$('skin').onchange=e=>{const f=e.target.files[0];if(!f)return;const url=URL.createObjectURL(f);new T.TextureLoader().load(url,t=>{URL.revokeObjectURL(url);face.map?.dispose();t.colorSpace=T.SRGBColorSpace;t.repeat.setScalar(1/4.8);t.offset.set(.5,.5);face.map=t;face.color.set(0xffffff);face.needsUpdate=true;$('finish').textContent=f.name;for(const sw of $('colors').children)sw.setAttribute('aria-pressed',false)},undefined,()=>{URL.revokeObjectURL(url);$('finish').textContent='Choose a readable image.'})};
reset();update();$('reset').onclick=reset;
const canvas=renderer.domElement;canvas.tabIndex=0;canvas.ariaLabel='Rotatable 3D model';
let start;
canvas.onpointerdown=e=>{canvas.setPointerCapture(e.pointerId);start=[e.clientX,e.clientY]};
canvas.onpointermove=e=>{if(canvas.hasPointerCapture(e.pointerId)){body.rotation.y+=e.movementX*.012;body.rotation.x+=e.movementY*.012}};
canvas.onpointerup=e=>{
 if(!start||Math.hypot(e.clientX-start[0],e.clientY-start[1])>5)return;
 const b=canvas.getBoundingClientRect(),ray=new T.Raycaster();
 ray.setFromCamera(new T.Vector2((e.clientX-b.left)/b.width*2-1,1-(e.clientY-b.top)/b.height*2),camera);
 if(ray.intersectObject(button,true).length)testPress();
};
canvas.onkeydown=e=>{
 const delta={ArrowLeft:[0,-.1],ArrowRight:[0,.1],ArrowUp:[-.1,0],ArrowDown:[.1,0]}[e.key];
 if(delta){e.preventDefault();body.rotation.x+=delta[0];body.rotation.y+=delta[1]}
};
new ResizeObserver(()=>{
 const w=stage.clientWidth,h=stage.clientHeight,a=w/h,v=Math.max(3.7,3.1/a);
 camera.left=-v*a;camera.right=v*a;camera.top=v+.45;camera.bottom=-v+.45;
 camera.updateProjectionMatrix();renderer.setSize(w,h,false);
}).observe(stage);
renderer.setAnimationLoop(()=>{
 slider.position.x+=((locked?-.32:.32)-slider.position.x)*.2;
 battery.position.z+=((mode==='explode'?.10:-.54)-battery.position.z)*.15;
 lid.position.z+=((mode==='explode'?3.50:0)-lid.position.z)*.15;
 board.position.z+=((mode==='explode'?1.85:0)-board.position.z)*.15;
 button.position.z=performance.now()<pressed?.31:.38;
 renderer.render(scene,camera);
});
