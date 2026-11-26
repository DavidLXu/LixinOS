// One local commander and the upstream deterministic skirmish opponent.
import { advance, OrderKind, isCellVisible, isCellExplored, readyToPlaceOf, checkPlacement } from '@vanguard/sim';
import { createBot } from '@vanguard/game';
import { AiDb, parseIni } from '@vanguard/formats';
import { buildSandbox, withOrderIndices } from 'client-sim';
import { blockTerrainObjectCells, densifyPassability } from 'client-net';

const {map: terrain, rules: wire, ai, mapName} = globalThis.match;
const {rect, sprite, text, log} = globalThis;
const {width, height} = terrain;
const cells = blockTerrainObjectCells(densifyPassability(Uint8Array.from(terrain.cells), width, height), width, height, terrain.terrain);
const map = {width,height,sim:{width,height,cells}, oreByCell:Uint16Array.from(terrain.oreByCell),waypoints:terrain.startWaypoints};
const rules = {general:wire.general};
for (const key of ['buildings','units','weapons','warheads','projectiles']) rules[key] = new Map(Object.entries(wire[key]));
const {world,spawns} = buildSandbox(map,rules);
const bot = createBot({playerIndex:1,seed:0x5b02,rules,aiDb:AiDb.build(parseIni(ai)),aiProfile:'normal'});
let camX = spawns[0].cellX, camY = spawns[0].cellY;
const props = new Map(terrain.terrain.map(obj=>[obj.y*width+obj.x,obj.type]));
const live = () => [...world.entities.valuesInOrder()].filter(e=>e.state!==2);
const type = e => world.types.nameOf(e.typeId);
let selection = new Set([live().find(e=>e.owner===0).id]);
let orders = [], placing = '', attackMove = false, message = 'D deploys your MCV';
const catalog = [['NAPOWR','Power plant'],['NAREFN','Refinery'],['NAHAND','Barracks'],['NAWEAP','War factory'],['E2','Conscript'],['HTNK','Rhino tank'],['NARADR','Radar'],['NALASR','Sentry gun'],['HARV','Harvester']];
function issue(kind, fields) { orders.push({kind,playerIndex:0,...fields}); }
function tick() {
    advance(world,withOrderIndices([...orders,...bot.onTick(world)]));
    orders = [];
}
function deploy() {
    for (const e of live()) if (selection.has(e.id) && type(e)==='SMCV') {
        issue(OrderKind.DeployEntity,{entityId:e.id});
        message = 'Deploying MCV';
    }
}
function produce(index) {
    const [typeName,label] = catalog[index];
    if (readyToPlaceOf(world,0).has(typeName)) {
        placing=typeName; message='Click near your base'; return;
    }
    issue(OrderKind.StartProduction,{typeName});
    message = label + ' requested';
}
let vw=0, vh=0, pressed=false, startX=0,startY=0;
function screen(x,y) {
    return [Math.round((x-camX-y+camY)*24+vw/2), Math.round((x-camX+y-camY)*12+vh/2)];
}
function cell(x,y) {
    const a=(x-vw/2)/48, b=(y-vh/2)/24;
    return [Math.round(camX+a+b),Math.round(camY-a+b)];
}
function foot(e) {
    const foundation=rules.buildings.get(type(e))?.foundation;
    return foundation ? screen(e.cellX+(foundation.w-1)/2,e.cellY+(foundation.h-1)/2) : screen(e.posX/256-.5,e.posY/256-.5);
}
function shown(e) { return e.owner===0 || isCellVisible(world,0,e.cellX,e.cellY); }
function click(mx,my) {
    if (mx>=vw) {
        const index=Math.floor((my-92)/31);
        if (index>=0 && index<catalog.length) produce(index);
        return;
    }
    const [x,y]=cell(mx,my);
    if(x<0 || y<0 || x>=width || y>=height) return;
    if(placing) {
        if(!checkPlacement(world,0,placing,x,y).ok) {message='Invalid building site'; return;}
        issue(OrderKind.PlaceBuilding,{typeName:placing,cellX:x,cellY:y});
        placing='';message='Building placed';return;
    }
    const visible=live().filter(shown).sort((a,b)=>foot(b)[1]-foot(a)[1]);
    const picked=visible.find(e=>{
        const [sx,sy]=foot(e), large=rules.buildings.has(type(e));
        return Math.abs(mx-sx)<(large?32:16) && my>=sy-(large?45:27) && my<=sy+7;
    });
    if(picked?.owner===0) { selection=new Set([picked.id]);message=type(picked)+' selected';return;}
    for(const e of live()) if(selection.has(e.id) && rules.units.has(type(e))) {
        if(picked) issue(OrderKind.AttackEntity,{entityId:e.id,targetId:picked.id});
        else if(attackMove) issue(OrderKind.AttackMove,{entityId:e.id,targetX:x,targetY:y});
        else if(type(e)==='HARV' && world.game.global.oreByCell[y*width+x])
            issue(OrderKind.Harvest,{entityId:e.id,cellX:x,cellY:y});
        else issue(OrderKind.MoveEntity,{entityId:e.id,targetX:x*256+128,targetY:y*256+128});
        message=picked?'Attacking':attackMove?'Advancing to attack':'Order received';
    }
    attackMove=false;
}
function handle(mx,my,down,key) {
    if(key===100 || key===68 || key===32) deploy();
    if(key>=49 && key<49+catalog.length) produce(key-49);
    if(key===102) {camX=spawns[0].cellX; camY=spawns[0].cellY;}
    if(key===27) {placing='';attackMove=false;}
    if(key===97 || key===65) {placing='';attackMove=true;message='Click attack route';}
    const dx=key===104||key===304?-1:key===108||key===303?1:0;
    const dy=key===107||key===301?-1:key===106||key===302?1:0;
    camX=Math.max(0,Math.min(width-1,camX+dx+dy)); camY=Math.max(0,Math.min(height-1,camY-dx+dy));
    if(down && !pressed) {startX=mx;startY=my;}
    if(!down && pressed && mx>=0 && my>=0) {
        if(startX<vw && mx<vw && Math.max(Math.abs(mx-startX),Math.abs(my-startY))>9) {
            selection.clear();
            for(const e of live()) {
                const [x,y]=foot(e);
                if(e.owner===0 && x>=Math.min(mx,startX) && x<=Math.max(mx,startX) && y>=Math.min(my,startY) && y<=Math.max(my,startY)) selection.add(e.id);
            }
            message=selection.size+' selected';
        } else click(mx,my);
    }
    pressed=down;
}
function outline(x,y,w,h,color) {
    rect(x,y,w,1,color);rect(x,y+h-1,w,1,color);rect(x,y,1,h,color);rect(x+w-1,y,1,h,color);
}
globalThis.frame=(w,h,mx,my,down,key,steps)=>{
    vw=Math.max(80,w-185);vh=h;handle(mx,my,down,key);
    for(let i=0;i<steps && !world.game.global.matchOver;i++) tick();
    rect(0,0,w,h,0x101719);
    const corners=[cell(-48,-60),cell(vw+48,-60),cell(-48,h+60),cell(vw+48,h+60)];
    const lo=(axis)=>Math.max(0,Math.min(...corners.map(c=>c[axis]))-1);
    const hi=(axis,limit)=>Math.min(limit-1,Math.max(...corners.map(c=>c[axis]))+1);
    const x0=lo(0),x1=hi(0,width),y1=hi(1,height);
    const objects=[];
    for(let y=lo(1);y<=y1;y++) for(let x=x0;x<=x1;x++){
        const [sx,sy]=screen(x,y);
        if(sx< -24||sx>vw+24||sy< -12||sy>h+12||!isCellExplored(world,0,x,y)) continue;
        const k=y*width+x;
        sprite('g'+terrain.tileNum[k]+'_'+terrain.subTile[k],sx,sy+12);
        if(world.game.global.oreByCell[k]) {rect(sx-5,sy,4,2,0xbfa951);rect(sx+2,sy+2,3,2,0xdfbd66);}
        if(props.has(k)) objects.push({x:sx,y:sy,name:'p'+props.get(k)});
    }
    for(const entity of live().filter(shown)) {
        const [x,y]=foot(entity);
        objects.push({x,y,entity});
    }
    objects.sort((a,b)=>a.y-b.y);
    for(const object of objects) {
        const {x:sx,y:sy,entity:e}=object;
        if(!e) {sprite(object.name,sx,sy+3);continue;}
        const name=type(e);
        if(sx< -48||sx>vw+48||sy<0||sy>h+84) continue;
        const building=rules.buildings.has(name);
        sprite(name+(building?0:Math.floor((e.facing+4096)/8192)%8),sx,sy+4);
        const color=e.owner===0?0x57bb83:0xe57366;
        rect(sx-6,sy+4,12,2,color);
        if(selection.has(e.id)) {
            const hp=(rules.buildings.get(name)||rules.units.get(name)).strength;
            rect(sx-13,sy-29,26,3,0x3d3936); rect(sx-13,sy-29,Math.ceil(26*e.hp/hp),3,color);
        }
    }
    for(let i=world.events.length-1;i>=0;i--) {
        const event=world.events[i];
        if(world.tick-event.tick>3) break;
        if(event.kind!=='ShotFired' && event.kind!=='ProjectileImpact') continue;
        const pos=/\bx=(-?\d+) y=(-?\d+)/.exec(event.detail);
        if(!pos) continue;
        const x=+pos[1]/256,y=+pos[2]/256;
        if(!isCellVisible(world,0,Math.floor(x),Math.floor(y))) continue;
        const [sx,sy]=screen(x-.5,y-.5);
        const r=event.kind==='ShotFired'?2:4;
        rect(sx-r,sy-6,r*2,3,0xffa145);rect(sx-1,sy-7-r,3,r*2,0xffdf78);
    }
    if(pressed && mx>=0 && mx<vw && startX<vw) outline(Math.min(mx,startX),Math.min(my,startY),Math.abs(mx-startX)+1,Math.abs(my-startY)+1,0x77dcad);
    if(placing && mx>=0 && mx<vw){
        const [x,y]=cell(mx,my),[sx,sy]=screen(x,y);
        outline(sx-12,sy-6,24,12,checkPlacement(world,0,placing,x,y).ok?0x77dcad:0xed5743);
    }
    rect(vw,0,w-vw,h,0x202728);
    const label=(y,s,c=0xece7d6)=>text(vw+8,y,s,c);
    label(5,'SOVIET COMMAND',0xe8ba78);
    const player=world.player(0);
    label(31,'Credits '+player.credits);
    label(56,'Power '+player.powerProduced+'/'+player.powerDrained);
    const ready=readyToPlaceOf(world,0);
    const qs=world.game.players[0].production.flatMap(q=>q.items);
    catalog.forEach(([name,title],i)=>{
        const y=92+i*31;
        const active=qs.find(item=>world.types.nameOf(item.typeId)===name);
        rect(vw+5,y,w-vw-10,27,ready.has(name)?0x42765b:active?0x71583b:0x353e40);
        if(active) {const cost=(rules.units.get(name)||rules.buildings.get(name)).cost;rect(vw+5,y+24,Math.round((w-vw-10)*(1-active.remainingCost/cost)),3,0xdfab69);}
        label(y+2,(i+1)+' '+title+(ready.has(name)?' *':''));
    });
    label(92+catalog.length*31+7,message.slice(0,19),0xe8ba78);
    rect(0,h-25,vw,25,0x1c2526);
    text(7,h-24,world.game.global.matchOver?(world.game.global.winner===0?'Victory   Q exit':'Defeat   Q exit'):'D deploy   A attack-move   Arrows pan   Q exit',0xefeadc);
};
globalThis.check=()=>{
    const mcv=live().find(e=>e.owner===0),dest=spawns[0].cellX+1;
    issue(OrderKind.MoveEntity,{entityId:mcv.id,targetX:dest*256+128,targetY:mcv.posY});
    for(let i=0;i<80 && mcv.cellX!==dest;i++) tick();
    if(mcv.cellX!==dest) throw Error('MCV did not move');
    deploy();tick();
    if(!live().some(e=>type(e)==='NACNST'&&e.owner===0)) throw Error('MCV did not deploy');
    function build(name) {
        issue(OrderKind.StartProduction,{typeName:name});tick();
        for(let i=0;i<2000 && !readyToPlaceOf(world,0).has(name);i++) tick();
        if(!readyToPlaceOf(world,0).has(name)) throw Error(name+' did not finish');
        for(let y=0;y<height;y++) for(let x=0;x<width;x++) if(checkPlacement(world,0,name,x,y).ok) {
            issue(OrderKind.PlaceBuilding,{typeName:name,cellX:x,cellY:y});tick();
            if(!live().some(e=>e.owner===0&&type(e)===name)) throw Error('Placement failed: '+name);
            return;
        }
        throw Error('No valid site for '+name);
    }
    build('NAPOWR');
    const enemyBase=live().find(e=>e.owner===1&&type(e)==='NACNST');
    if(!enemyBase) throw Error('Computer did not deploy');
    build('NAHAND');
    issue(OrderKind.StartProduction,{typeName:'E2'});tick();
    const conscript=()=>live().find(e=>e.owner===0&&type(e)==='E2');
    for(let i=0;i<2000 && !conscript();i++) tick();
    const infantry=conscript();
    if(!infantry) throw Error('No conscript left the barracks');
    issue(OrderKind.AttackMove,{entityId:infantry.id,targetX:enemyBase.cellX-2,targetY:enemyBase.cellY-2});
    for(let i=0;i<2400;i++) {
        tick();
        if(world.events.some(event=>event.kind==='ShotFired' && event.detail.startsWith('a='+infantry.id+' '))) {
            log('RA2 verified movement, both construction yards, power plant, barracks and infantry combat');
            return;
        }
    }
    throw Error('Conscript never fired');
};
