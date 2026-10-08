// Every event the door can log, labelled as the server's /table labels it
// (petdoor-logserver.py EVENT_LABEL). An event missing here used to render as
// "undefined" — which is how a door moved by hand vanished from this page while
// sitting correctly in the database.
const EV={OPEN:"came in",CLOSE:"went out",BOOT:"restarted",REFUSED:"refused",FIX_GOT:"beacon found",FIX_LOST:"beacon lost",STALLED:"did not complete its travel",
  UNCOMMANDED:"moved, not by PetDoor",RETRY:"pressed again",GAVE_UP:"gave up closing",WAKE:"woke the controller",NO_MOVE:"did not move at all",
  MAINT:"maintenance mode",HOLD:"held in position",CONSOLE:"network console",BEACON_LOW:"beacon battery",SENSOR_FAULT:"sensor fault",SENSOR_OK:"sensor recovered"};
// The fallback is the type string as uploaded, so it is escaped: it reaches
// innerHTML, and a signed upload is still not a reason to trust its markup.
const escHtml=v=>String(v==null?"":v).replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
const evLabel=t=>EV[t]||escHtml(t);
// UNCOMMANDED's detail is where the door ended up: 1/2 when a limit switch
// MEASURED it, 11/12 (kUncommandedInferred + state) when it was INFERRED from
// how long the vibration sensor felt it move, with that duration in src.
// Shown differently on purpose — see CLAUDE.md invariant 20.
function uncommandedDetail(e){
  const inferred=e.detail>=10, st=inferred?e.detail-10:e.detail;
  const where=st===1?"open":st===2?"closed":"state "+st;
  const how=inferred?(e.src?`inferred from ${(e.src/1000).toFixed(1)} s of movement`:"inferred from the movement")
                    :"a limit switch saw it";
  return `<strong data-u="c-fault">moved to ${where}, nothing commanded it</strong> <span data-u="c-ink3">(${how})</span>`;
}
const RESET={1:"power-on",3:"software",4:"panic",5:"interrupt watchdog",6:"task watchdog",7:"watchdog",9:"brownout",};
const REFUSE={1:"already there",2:"too soon after last move",3:"boot grace period"};
// OPEN and CLOSE spend `detail` on the ActuationSource (petdoor/door.h), so
// without this the Detail column was BLANK for every door movement — which is
// how a page full of correct beacon opens read as "nothing tagged the beacon".
// Worse, it rendered a fail-safe reversal after a stalled close identically to
// an ordinary beacon open, and those mean very different things: see CLAUDE.md
// invariant 20 on why two different facts must not look the same here.
const SRC={0:"beacon",1:"console",2:"network",3:"fail-safe reversal"};
// Maintenance window durations offered on the dashboard, in minutes. The
// firmware accepts any value up to MAINT_MAX_MS (4 h) and REFUSES anything
// longer rather than shortening it (maintenance.cpp: "Silently shortening a
// window somebody asked for" is the thing it avoids). So 240 is the last entry
// that works, and offering 480 here would just produce a refusal in the
// acknowledgement.
const MAINT_CHOICES=[15,30,60,120,240];
const maintLabel=m=>m<60?`${m} min`:(m%60?`${(m/60).toFixed(1)} h`:`${m/60} h`);
let events=[], commands=[], selected=null;
let pending=[], controlOn=false, devices=[], busy=false;

const $=id=>document.getElementById(id);
const pad=n=>String(n).padStart(2,"0");
/* Everything below renders in the VIEWER'S local time, not UTC.

   The door uploads UTC epochs, and showing them as UTC is tempting because it
   needs no configuration. But every chart on this page answers a question about
   a human day — when he first goes out, what the daily rhythm looks like, which
   hours are busy — and a UTC day is the wrong day for anyone who does not live
   on it. At UTC-7 an evening outing at 18:38 reports as "01:38" and lands on
   the following row, so the evening walk shows up as tomorrow's dawn.

   Local time is what the reader means by "a day", so use it consistently. */
const fmtClock=e=>{const d=new Date(e*1000);return pad(d.getHours())+":"+pad(d.getMinutes())};
const fmtDate=e=>{const d=new Date(e*1000);return d.getFullYear()+"-"+pad(d.getMonth()+1)+"-"+pad(d.getDate())};
const dayKey=e=>fmtDate(e);
const secsIntoDay=e=>{const d=new Date(e*1000);return d.getHours()*3600+d.getMinutes()*60+d.getSeconds()};
function dur(s){if(s<60)return s+"s";const m=Math.round(s/60);if(m<60)return m+"m";const h=Math.floor(m/60);return h+"h "+pad(m%60)+"m";}

function parseCsv(text){
  const out=[];
  for(const raw of text.split(/\r?\n/)){
    const l=raw.trim(); if(!l||l.startsWith("epoch,"))continue;
    const p=l.split(","); if(p.length<6)continue;
    const ev={epoch:+p[0]||0,uptime:+p[1]||0,boot:+p[2]||0,type:p[3].trim(),detail:+p[4]||0,rssi:+p[5]||0};
    if(!EV[ev.type])continue;
    out.push(ev);
  }
  return out;
}
const idOf=e=>e.boot+"|"+e.uptime+"|"+e.type+"|"+e.epoch;
function merge(incoming){
  const seen=new Set(events.map(idOf));
  let added=0;
  for(const e of incoming) if(!seen.has(idOf(e))){events.push(e);seen.add(idOf(e));added++;}
  events.sort((a,b)=>(a.epoch||0)-(b.epoch||0)||a.boot-b.boot||a.uptime-b.uptime);
  return added;
}

/* Pair each exit with the next return, so "outside" is a span not two dots. */
/* A trip is "closed behind him, opened again when he came back".

   The door cannot tell which side of it the animal is on — it has no direction
   sensing, only proximity — so it CLOSES twice per round trip: once when he
   leaves and again once he has come in and wandered off. Pairing every CLOSE
   with the next OPEN therefore counts the time he spends INDOORS as a trip
   outside, which is where "outside 22h a day" and multi-hour "trips" come from.

   So alternate instead: the first CLOSE after an opening means he is out, and
   the next OPEN means he is back. A second CLOSE while he is already counted as
   home starts nothing. This is an inference, not a measurement — if an upload
   is lost the parity can slip until the next quiet night resynchronises it. */
function trips(){
  /* One round trip produces FOUR events, because the door reacts to the beacon
     arriving and leaving on both legs of the journey:

        OPEN   he is at the door, on his way out
        CLOSE  he has gone — this is when he is outside      <- trip starts
        OPEN   he is back at the door                        <- trip ends
        CLOSE  he has wandered off indoors

     So it is every OTHER close that begins a spell outside. Pairing each close
     with the next open counts the indoor stretch as a trip too, which doubles
     the trip count and produces "outside 22h a day". */
  const t=[], dated=events.filter(e=>e.epoch>0);
  let closes=0, since=null;
  for(const e of dated){
    if(e.type==="CLOSE"){
      if(closes % 2 === 0) since=e.epoch;   // he has just left
      closes++;
    }else if(e.type==="OPEN" && since!==null){
      t.push({out:since,in:e.epoch}); since=null;
    }
  }
  if(since!==null) t.push({out:since,in:null});   // still out right now
  return t;
}

const median=a=>{if(!a.length)return 0;const b=[...a].sort((x,y)=>x-y);const m=b.length>>1;
  return b.length%2?b[m]:Math.round((b[m-1]+b[m])/2);};

/* Reusable column chart. One scale places bars, ticks and labels; every axis
   label names a value the chart actually reaches. */
function bars(host,data,opts){
  const o=Object.assign({h:170,color:"var(--in)",fmt:v=>v,label:d=>d.label,every:1,unit:""},opts||{});
  const el=$(host);
  if(!data.length||data.every(d=>!d.v)){el.innerHTML='<div class="empty">Not enough data yet</div>';return;}
  const W=640,L=34,R=10,T=14,B=30,plot=W-L-R,ph=o.h-T-B;
  const max=Math.max(...data.map(d=>d.v))||1;
  const nice=max<=4?max:Math.ceil(max/4)*4;
  const bw=Math.max(3,Math.min(38,plot/data.length-6));
  const step=plot/data.length;
  const y=v=>T+ph-(v/nice)*ph;
  let g="";
  for(let i=0;i<=2;i++){
    const v=nice*i/2, yy=y(v);
    g+=`<line x1="${L}" y1="${yy}" x2="${L+plot}" y2="${yy}" stroke="var(--line-2)" stroke-width="1"/>`;
    g+=`<text x="${L-7}" y="${yy+3.5}" text-anchor="end" font-size="10" fill="var(--ink-3)" font-family="IBM Plex Mono,monospace">${o.fmt(v)}</text>`;
  }
  data.forEach((d,i)=>{
    const cx=L+step*i+step/2, hgt=Math.max(d.v?2:0,ph-(y(d.v)-T));
    if(d.v) g+=`<rect x="${cx-bw/2}" y="${y(d.v)}" width="${bw}" height="${hgt}" rx="3" fill="${d.color||o.color}"><title>${o.label(d)}: ${o.fmt(d.v)}${o.unit}</title></rect>`;
    if(i%o.every===0) g+=`<text x="${cx}" y="${T+ph+16}" text-anchor="middle" font-size="10" fill="var(--ink-3)" font-family="IBM Plex Mono,monospace">${d.tick??d.label}</text>`;
  });
  g+=`<line x1="${L}" y1="${T+ph}" x2="${L+plot}" y2="${T+ph}" stroke="var(--line)" stroke-width="1"/>`;
  el.innerHTML=`<svg viewBox="0 0 ${W} ${o.h}" role="img" aria-label="${o.aria||host}">${g}</svg>`;
}

function renderDurations(){
  const done=trips().filter(t=>t.in).map(t=>t.in-t.out);
  if(!done.length){$("durations").innerHTML='<div class="empty">No completed trips yet</div>';$("durnote").textContent="";$("durhint").textContent="";return;}
  const edges=[0,5,10,15,30,45,60,90,120,1e9];
  const names=["<5m","5-10m","10-15m","15-30m","30-45m","45-60m","1-1.5h","1.5-2h","2h+"];
  const buckets=names.map((n,i)=>({label:n,tick:n,v:0}));
  for(const d of done){const m=d/60;
    for(let i=0;i<edges.length-1;i++) if(m>=edges[i]&&m<edges[i+1]){buckets[i].v++;break;}}
  bars("durations",buckets,{color:"var(--out)",h:180,every:1,unit:" trips",
    label:d=>d.label+" outside",aria:"How many trips fell into each duration band"});
  const med=median(done), mean=Math.round(done.reduce((a,b)=>a+b,0)/done.length);
  $("durhint").textContent=done.length+" completed trips";
  const skew=mean>med*1.4;
  $("durnote").innerHTML=`Typical trip is <strong>${dur(med)}</strong> (median). `
    +(skew?`The average is ${dur(mean)}, pulled up by a few long ones — the median is the honest number here.`
          :`The average, ${dur(mean)}, is close to it, so trips are fairly consistent.`);
}

function renderPerDay(){
  const dated=events.filter(e=>e.epoch>0);
  if(!dated.length){$("perday").innerHTML='<div class="empty">No timestamped events yet</div>';return;}
  const by={};
  for(const t of trips()) by[dayKey(t.out)]=(by[dayKey(t.out)]||0)+1;
  const days=[...new Set(dated.map(e=>dayKey(e.epoch)))].sort().slice(-14);
  const data=days.map(d=>({label:d,tick:d.slice(8),v:by[d]||0}));
  const avg=data.reduce((a,b)=>a+b.v,0)/(data.length||1);
  bars("perday",data,{color:"var(--in)",h:170,every:Math.ceil(days.length/7),unit:" trips",
    label:d=>d.label,aria:"Trips outside on each of the last days"});
  $("trendhint").textContent=avg?avg.toFixed(1)+" a day on average":"";
}

function renderHourly(){
  const t=trips();
  if(!t.length){$("hourly").innerHTML='<div class="empty">No trips yet</div>';return;}
  const h=Array.from({length:24},(_,i)=>({label:pad(i)+":00",tick:i%6===0?pad(i):"",v:0}));
  for(const x of t) h[new Date(x.out*1000).getHours()].v++;
  bars("hourly",h,{color:"var(--out)",h:170,every:1,unit:" trips",
    label:d=>"trips starting "+d.label,aria:"Trips by hour of day"});
}

function renderSignal(){
  const pts=events.filter(e=>e.type==="OPEN"&&e.rssi&&e.epoch>0).slice(-40);
  const el=$("signal");
  if(pts.length<3){el.innerHTML='<div class="empty">Not enough returns recorded yet</div>';$("signote").textContent="";return;}
  const W=640,L=38,R=12,T=14,B=26,plot=W-L-R,ph=120;
  const vals=pts.map(p=>p.rssi), lo=Math.min(...vals)-4, hi=Math.max(...vals)+4;
  const x=i=>L+(i/(pts.length-1))*plot, y=v=>T+ph-((v-lo)/(hi-lo))*ph;
  let g="",d="";
  for(const v of [hi,Math.round((hi+lo)/2),lo]){
    g+=`<line x1="${L}" y1="${y(v)}" x2="${L+plot}" y2="${y(v)}" stroke="var(--line-2)" stroke-width="1"/>`;
    g+=`<text x="${L-7}" y="${y(v)+3.5}" text-anchor="end" font-size="10" fill="var(--ink-3)" font-family="IBM Plex Mono,monospace">${Math.round(v)}</text>`;
  }
  pts.forEach((p,i)=>{d+=(i?"L":"M")+x(i)+" "+y(p.rssi);});
  g+=`<path d="${d}" fill="none" stroke="var(--in)" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>`;
  pts.forEach((p,i)=>{g+=`<circle cx="${x(i)}" cy="${y(p.rssi)}" r="${i===pts.length-1?4.5:2.5}" fill="var(--in)" stroke="var(--panel)" stroke-width="${i===pts.length-1?2:0}"><title>${fmtDate(p.epoch)} ${fmtClock(p.epoch)} · ${p.rssi} dBm</title></circle>`;});
  g+=`<text x="${L}" y="${T+ph+18}" font-size="10" fill="var(--ink-3)" font-family="IBM Plex Mono,monospace">${fmtDate(pts[0].epoch)}</text>`;
  g+=`<text x="${L+plot}" y="${T+ph+18}" text-anchor="end" font-size="10" fill="var(--ink-3)" font-family="IBM Plex Mono,monospace">${fmtDate(pts[pts.length-1].epoch)}</text>`;
  el.innerHTML=`<svg viewBox="0 0 ${W} ${T+ph+B}" role="img" aria-label="Beacon signal strength at each return">${g}</svg>`;
  const half=Math.floor(pts.length/2);
  const early=median(vals.slice(0,half)), late=median(vals.slice(half));
  const drop=early-late;
  $("signote").innerHTML = drop>=6
    ? `<strong data-u="c-fault">Signal has dropped ${drop} dBm</strong> across this window. That usually means the beacon battery is going — or it has moved further from the door. Worth checking before it starts missing.`
    : `Steady within ${Math.abs(drop)} dBm, so the beacon is holding up. A sustained drop of 6 dBm or more is the early warning for a flat battery.`;
}

function renderStats(){
  const dated=events.filter(e=>e.epoch>0), tr=trips();
  const done=tr.filter(t=>t.in), days=new Set(dated.map(e=>dayKey(e.epoch))).size||1;
  const total=done.reduce((s,t)=>s+(t.in-t.out),0);
  const longest=done.reduce((m,t)=>Math.max(m,t.in-t.out),0);
  const faults=events.filter(e=>e.type==="BOOT"&&e.detail===9).length;
  const durs=done.map(t=>t.in-t.out);
  const med=median(durs);
  const perDay=tr.length/days;
  const outPerDay=Math.round(total/days);
  const firsts=[],lasts=[];
  for(const d of new Set(dated.map(e=>dayKey(e.epoch)))){
    const day=tr.filter(t=>dayKey(t.out)===d).map(t=>secsIntoDay(t.out));
    if(day.length){firsts.push(Math.min(...day));lasts.push(Math.max(...day));}
  }
  const hhmm=s2=>pad(Math.floor(s2/3600))+":"+pad(Math.round((s2%3600)/60));
  const cards=[
    ["Trips a day",perDay?perDay.toFixed(1):"—",`${tr.length} over ${days} day${days>1?"s":""}`],
    ["Typical trip",med?dur(med):"—","median, not average"],
    ["Longest trip",longest?dur(longest):"—","single outing"],
    ["Outside a day",outPerDay?dur(outPerDay):"—","total time out"],
    ["First out",firsts.length?hhmm(median(firsts)):"—","typical start"],
    ["Last out",lasts.length?hhmm(median(lasts)):"—","typical finish"],
  ];
  if(faults) cards.push(["Brownouts",faults,"check the power supply"]);
  $("stats").innerHTML=cards.map(([k,v,n])=>
    `<div class="stat"><div class="k">${k}</div><div class="v mono">${v}</div><div class="n">${n}</div></div>`).join("");
}

function renderStrip(){
  const dated=events.filter(e=>e.epoch>0);
  const host=$("strip");
  if(!dated.length){host.innerHTML='<div class="empty">No timestamped events yet. The door records real times once it has synced its clock over WiFi.</div>';return;}
  const days=[...new Set(dated.map(e=>dayKey(e.epoch)))].sort().slice(-14);
  const L=74,R=18,T=28,rowH=34,H=T+days.length*rowH+18,W=900,plot=W-L-R;
  const x=s=>L+(s/86400)*plot;
  const tr=trips();
  let g="";
  // night shading 20:00-06:00 — the circadian frame the data sits in
  g+=`<rect x="${L}" y="${T}" width="${x(6*3600)-L}" height="${days.length*rowH}" fill="var(--night)"/>`;
  g+=`<rect x="${x(20*3600)}" y="${T}" width="${L+plot-x(20*3600)}" height="${days.length*rowH}" fill="var(--night)"/>`;
  for(let h=0;h<=24;h+=3){
    g+=`<line x1="${x(h*3600)}" y1="${T}" x2="${x(h*3600)}" y2="${T+days.length*rowH}" stroke="var(--line-2)" stroke-width="1"/>`;
    g+=`<text x="${x(h*3600)}" y="${T-9}" text-anchor="middle" font-size="10.5" fill="var(--ink-3)" font-family="IBM Plex Mono,monospace">${pad(h)}</text>`;
  }
  days.forEach((d,i)=>{
    const y=T+i*rowH+rowH/2;
    const lbl=new Date(d+"T00:00:00Z");
    g+=`<text x="${L-12}" y="${y+4}" text-anchor="end" font-size="11.5" fill="var(--ink-2)" font-family="IBM Plex Mono,monospace">${["Sun","Mon","Tue","Wed","Thu","Fri","Sat"][lbl.getDay()]} ${d.slice(5)}</text>`;
    g+=`<line x1="${L}" y1="${y}" x2="${L+plot}" y2="${y}" stroke="var(--line-2)" stroke-width="1"/>`;
    // outside spans
    for(const t of tr){
      if(dayKey(t.out)!==d)continue;
      const x1=x(secsIntoDay(t.out));
      const x2=t.in&&dayKey(t.in)===d?x(secsIntoDay(t.in)):L+plot;
      g+=`<rect x="${x1}" y="${y-8}" width="${Math.max(3,x2-x1)}" height="16" rx="4" fill="var(--out)" opacity="0.30" stroke="var(--out)" stroke-width="1.25"/>`;
    }
    for(const e of dated.filter(e=>dayKey(e.epoch)===d)){
      const cx=x(secsIntoDay(e.epoch));
      if(e.type==="CLOSE") g+=`<circle cx="${cx}" cy="${y}" r="5" fill="var(--out)" stroke="var(--panel)" stroke-width="2"><title>Went out ${fmtClock(e.epoch)}</title></circle>`;
      else if(e.type==="OPEN") g+=`<circle cx="${cx}" cy="${y}" r="5" fill="var(--in)" stroke="var(--panel)" stroke-width="2"><title>Came in ${fmtClock(e.epoch)}</title></circle>`;
      else if(e.type==="UNCOMMANDED"){const st=e.detail>=10?e.detail-10:e.detail;
        g+=`<rect x="${cx-4.5}" y="${y-4.5}" width="9" height="9" transform="rotate(45 ${cx} ${y})" fill="var(--panel)" stroke="var(--fault)" stroke-width="2"><title>Moved to ${st===1?"open":st===2?"closed":"?"} with nothing commanding it ${fmtClock(e.epoch)}</title></rect>`;}
      else if(e.type==="BOOT") g+=`<rect x="${cx-2}" y="${y-9}" width="4" height="18" rx="1.5" fill="${e.detail===9?"var(--fault)":"var(--ink-3)"}"><title>${e.detail===9?"Brownout":"Restart"} ${fmtClock(e.epoch)}</title></rect>`;
    }
  });
  host.innerHTML=`<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Timeline of door events by hour for each day"><rect x="0" y="0" width="${W}" height="${H}" fill="none"/>${g}</svg>`;
}

function renderTable(){
  const host=$("tablewrap");
  if(!events.length){host.innerHTML='<div class="empty">Nothing imported yet. Load the example, or drop in a CSV from the door.</div>';$("evhint").textContent="";return;}
  const rows=events.slice().reverse().slice(0,60);
  $("evhint").textContent=events.length+" events"+(events.length>60?" · newest 60, scroll for more":"");
  const cls=t=>t==="OPEN"?"in":t==="CLOSE"?"out":(t==="REFUSED"||t==="UNCOMMANDED"||t==="GAVE_UP"||t==="SENSOR_FAULT")?"fault":"sys";
  host.innerHTML="<table><thead><tr><th>When</th><th>Event</th><th>Detail</th><th>Signal</th><th></th></tr></thead><tbody>"
   +rows.map((e,i)=>{
     const when=e.epoch?`<span class="mono">${fmtDate(e.epoch)}</span> <span class="mono" data-u="c-ink2">${fmtClock(e.epoch)}</span>`
                       :`<span class="mono" data-u="c-ink3">boot ${e.boot} · +${e.uptime}s</span>`;
     let d="";
     if(e.type==="BOOT") d=RESET[e.detail]||("reset "+e.detail);
     else if(e.type==="REFUSED") d=REFUSE[e.detail]||("reason "+e.detail);
     else if(e.type==="UNCOMMANDED") d=uncommandedDetail(e);
     else if(e.type==="OPEN"||e.type==="CLOSE") d=SRC[e.detail]||("source "+e.detail);
     else if(e.type==="HOLD") d=e.detail===2
          ? `<strong data-u="c-fault">HELD CLOSED — an animal outside cannot get in</strong>`
          : e.detail===1 ? "held OPEN — nothing automatic will close it"
                         : "released — the door decides for itself again";
     if(e.type==="BOOT"&&e.detail===9) d=`<strong data-u="c-fault">${d}</strong>`;
     // A fail-safe reversal is not an ordinary open: it says a close STALLED.
     if((e.type==="OPEN"||e.type==="CLOSE")&&e.detail===3) d=`<strong data-u="c-fault">${d}</strong>`;
     return `<tr><td>${when}</td><td><span class="pill ${cls(e.type)}"><i class="dot" data-u="bg-current"></i>${evLabel(e.type)}</span></td>`
      +`<td data-u="c-ink2">${d}</td><td class="mono" data-u="c-ink2">${e.rssi?e.rssi+" dBm":""}</td>`
      +`<td data-u="ta-r">${e.epoch?`<button class="cam" data-ep="${e.epoch}">Footage</button>`:""}</td></tr>`;
   }).join("")+"</tbody></table>";
  host.querySelectorAll("button[data-ep]").forEach(b=>b.onclick=()=>selectEvent(+b.dataset.ep));
}

function selectEvent(ep){
  selected=ep;
  const stamp=fmtDate(ep)+" "+fmtClock(ep)+"Z";
  for(const id of ["camOut","camIn"]){
    $(id).innerHTML=`<div><div class="mono" data-u="lead-strong">${stamp}</div>
      <div data-u="mt4">scrub here</div></div>`;
  }
  $("camsec").scrollIntoView({behavior:"matchMedia" in window && matchMedia("(prefers-reduced-motion: reduce)").matches?"auto":"smooth",block:"nearest"});
}

/* Everything the door reports about ITSELF, as opposed to what the animal did.
   The status line rides every upload; see docs/REMOTE-CONFIG.md. */
function parseStatus(s){
  const out={};
  (s||"").split(/\s+/).forEach(pair=>{
    const i=pair.indexOf("=");
    if(i>0) out[pair.slice(0,i)]=pair.slice(i+1);
  });
  return out;
}

/* Where the door stands, and since when.

   Two sources, and they answer different questions. The uploaded status line
   says what the door believes RIGHT NOW, but carries no timestamp. The event
   log carries timestamps but is history. So: take the state from the status
   line where there is one, and the "since" from the most recent event that
   agrees with it.

   The fallback matters more than it looks. Between pressing Open and the door's
   next status upload there is a window where the status line still says CLOSED
   — the door only rebuilds it when it reports — but the OPEN event has already
   arrived. Preferring a newer event over a staler status closes that window, so
   the page stops insisting the door is shut some minutes after it opened. */
function doorStanding(dev){
  const st=parseStatus(dev && dev.status);
  const last={};
  for(const e of events){
    if(!e.epoch) continue;
    if(e.type==="OPEN"||e.type==="CLOSE"){
      if(!last[e.type] || e.epoch>last[e.type]) last[e.type]=e.epoch;
    }
  }
  const newest = (last.OPEN||0)>=(last.CLOSE||0) ? "OPEN" : "CLOSED";
  const newestAt = Math.max(last.OPEN||0, last.CLOSE||0);

  // An event that postdates the door's last upload is fresher than the status
  // line, which was built before that upload was sent.
  let state = st.door || null;
  if(newestAt && dev && dev.last_seen && newestAt >= dev.last_seen) state = newest;
  if(!state && newestAt) state = newest;

  // THE FASTEST SIGNAL: a door command the door has COLLECTED.
  //
  // This is the same fact that clears the "waiting for the door" list — the
  // command stopped being pending, so the door has it. Ignoring it for the
  // status while acting on it for the queue is the page contradicting itself.
  //
  // It is also the earliest thing we can know. The status line inside the very
  // upload that delivered the command was built BEFORE the command was
  // applied, so it necessarily shows the old state; the corrected one cannot
  // arrive until the following upload. `delivered` and `last_seen` are both
  // server-side stamps set by that same request, which is what makes them
  // safely comparable — unlike event epochs, which come off the door's clock.
  //
  // Manual commands call forcePulse*(), which bypasses the "already in that
  // state" and lockout checks, so a delivered one did actuate.
  let cmdAt = 0, cmdState = null;
  for(const c of commands){
    if(!c.delivered) continue;
    const verb = String(c.command||"").toLowerCase();
    if(verb!=="door open" && verb!=="door close") continue;
    if(c.delivered >= cmdAt){ cmdAt = c.delivered; cmdState = verb==="door open"?"OPEN":"CLOSED"; }
  }
  if(cmdState && dev && dev.last_seen && cmdAt >= dev.last_seen) state = cmdState;

  if(!state) return {state:null, since:0, label:""};

  // Prefer an event's timestamp for "since" — it is when the door actually
  // moved. Fall back to the command's delivery when the command is what told
  // us the state, because then no event has arrived to date it yet.
  let sinceAt = state==="OPEN" ? (last.OPEN||0) : (last.CLOSE||0);
  if(cmdState===state && cmdAt>sinceAt) sinceAt = cmdAt;
  let label="";
  if(sinceAt){
    const mins=Math.round((Date.now()/1000-sinceAt)/60);
    const ago = mins<1?"just now" : mins<60?`${mins} min` :
                mins<2880?`${Math.round(mins/60)} h` : `${Math.round(mins/1440)} days`;
    label = mins<1 ? `since ${fmtClock(sinceAt)}` : `since ${fmtClock(sinceAt)} \u00b7 ${ago}`;
  }
  return {state, since:sinceAt, label};
}

function renderDoors(devs){
  const host=$("doors");
  if(!devs.length){host.innerHTML="";return;}
  // Set below once we know whether any panel had anything to show.
  const now=Date.now()/1000;
  const panels=devs.map(d=>{
    const esc=t=>String(t==null?"":t).replace(/[<>&]/g,"");
    const mins=Math.round((now-(d.last_seen||0))/60);
    // A door that has not called in for hours is either off, off the network,
    // or failing to upload — worth flagging rather than leaving to be noticed.
    const stale=mins>180;
    const when=mins<1?"just now":mins<60?`${mins} min ago`:`${Math.round(mins/60)} h ago`;
    // Reboots climbing between uploads is the signature of a power problem.
    const boots=d.boots||0;
    const st=parseStatus(d.status);

    // With one door the banner already carries its firmware and last check-in,
    // so repeating them here is just noise three inches below. With several,
    // the banner can only describe one of them and each panel says its own.
    // With ONE door the masthead already carries its name, firmware, check-in,
    // signal and live state, so this panel keeps only what did not fit up
    // there. With several, the masthead can describe just the first, and each
    // panel has to identify and describe its own.
    const solo=devs.length===1;
    let rows=solo?"":`<div class="n">${esc(d.device||"door")}</div>`
      +`<div class="m">firmware v${esc(d.version||"?")}</div>`
      +`<div class="m${stale?" warn":""}">last heard ${when}${stale?" — check it":""}</div>`;

    if(st.rssi!==undefined){
      if(!solo){
        const present=st.present==="1";
        const stand=doorStanding(d);
        rows+=`<div class="live">`
          +`<span class="pill ${present?"in":"out"}">${present?"beacon present":"beacon away"}</span>`
          +`<span class="pill ${stand.state==="OPEN"?"in":""}">door ${esc(stand.state||st.door||"?")}`
          +(stand.label?` <span data-u="fw4 dim">${esc(stand.label)}</span>`:"")
          +`</span>`
          // A locked door will not open for the collar. That is the one state
          // worth shouting about, because from the outside it looks identical
          // to a door that is simply shut.
          +(st.locked==="1"?`<span class="pill lock">LOCKED — collar cannot open it</span>`:"")
          // Same reasoning as the lock pill, more so: during a maintenance
          // window the door ignores the collar entirely, which from out here
          // is indistinguishable from a door that has stopped working. Say how
          // long is left, because "it ends by itself" is the whole safety
          // argument and it is worthless if nobody can see the clock.
          +((Number(st.maint||0)||0)>0
             ? `<span class="pill lock">MAINTENANCE — not moving for ${Math.ceil(Number(st.maint)/60)} more min</span>`
             : "")
          // The hold. Deliberately has NO clock, because unlike maintenance it
          // does not end by itself — and showing a countdown it does not have
          // would be the most misleading thing on this page. Held CLOSED says
          // what it costs rather than naming itself.
          +(st.hold==="2"
             ? `<span class="pill lock">HELD CLOSED — an animal outside cannot get in, and this will not expire</span>`
             : st.hold==="1"
             ? `<span class="pill lock">HELD OPEN — nothing automatic will close it</span>`
             : "")
          +`</div>`;
      }

      // The numbers, as labelled cells rather than a run-on sentence. Signal
      // and distance are omitted when solo — they are in the masthead.
      const cell=(k,v)=>`<div><span class="k">${k}</span><span class="v">${v}</span></div>`;
      let grid="";
      if(st.rssi!==undefined) grid+=cell("Signal",esc(st.rssi)+" dBm");
      if(st.dist) grid+=cell("Distance","~"+esc(st.dist)+" m");
      if(st.gap) grid+=cell("Worst gap",esc(st.gap)+" ms");
      grid+=cell("Boots",String(boots));
      if(st.real&&st.real!=="none") grid+=cell("Measured",esc(st.real));
      if(d.door_ip) grid+=cell("Push to",`<span class="mono">${esc(d.door_ip)}</span>`);
      if(grid && !solo) rows+=`<div class="dgrid">${grid}</div>`;
    }

    // Health, and only when there is something to say about it.
    const warns=[];
    if(st.heap!==undefined && +st.heap<30000) warns.push(`low memory (${st.heap} bytes free)`);
    if(st.gap!==undefined && +st.gap>3000) warns.push(`worst gap ${st.gap} ms exceeds the 3 s fix timeout`);
    if(warns.length) rows+=`<div class="m warn">${warns.map(esc).join(" &middot; ")}</div>`;

    // Where to push an update, and whether this build would even take one.
    if(d.remote===0) rows+=`<div class="m warn">firmware predates remote commands — anything queued will wait</div>`;

    // With one door and nothing wrong, this panel has nothing left to say —
    // everything it carried is in the masthead. An empty card is worse than no
    // card, so it only appears when there is a warning to make.
    return rows.trim() ? `<div>${rows}</div>` : "";
  }).join("");
  host.innerHTML = panels ? `<div class="doors">${panels}</div>` : "";
}

/* The settings form.

   Ranges here mirror petdoor-logserver.py's WEB_COMMANDS, which mirror the
   firmware's own constants. Three copies of the same numbers is not ideal, but
   the alternative is a door that accepts a value the form offered and then
   refuses it five minutes later, which is worse. The door remains the
   authority; these bounds just fail fast and label the input.

   `c` is the key in the door's uploaded config line, which is how each field
   knows what it is currently set to. */
const SETTINGS=[
  {g:"Detection", rows:[
    {verb:"thresholds", lab:"Signal thresholds",
     sub:"dBm. Opens above the first, closes below the second",
     f:[{c:"enter",min:-120,max:0,ph:"-58"},{c:"exit",min:-120,max:0,ph:"-68"}]},
    {verb:"filter", lab:"Close filter",
     sub:"median window (odd, 1-15) and smoothing (0-1). Slower = steadier",
     f:[{c:"fwin",min:1,max:15,step:2,ph:"5"},{c:"falpha",min:0.01,max:1,step:0.01,ph:"0.30"}]},
    {verb:"openfilter", lab:"Open filter",
     sub:"must never be slower than the close filter, or a departure is noticed before an arrival",
     f:[{c:"owin",min:1,max:15,step:2,ph:"1"},{c:"oalpha",min:0.01,max:1,step:0.01,ph:"0.90"}]},
  ]},
  {g:"Timing", rows:[
    {verb:"dwell", lab:"Dwell and lockout",
     sub:"ms near before opening / ms away before closing / ms between actuations",
     f:[{c:"dopen",min:100,max:600000,ph:"1500"},{c:"dclose",min:100,max:600000,ph:"15000"},
        {c:"dmin",min:100,max:600000,ph:"5000"}]},
    {verb:"travel", lab:"Door travel time",
     sub:"ms. Drives the moving LED and the buzzer. 0 = announce nothing",
     f:[{c:"travel",min:0,max:120000,ph:"15000"}]},
  ]},
  {g:"Calling in", rows:[
    {verb:"upload", lab:"How often the door reports",
     sub:"ms quiet before uploading / minimum gap between uploads / heartbeat (0 = off). The middle one is the trade: lower means commands arrive sooner, at the cost of radio time the beacon scan would have had",
     f:[{c:"upsettle",min:5000,max:300000,ph:"60000"},
        {c:"upmin",min:60000,max:3600000,ph:"300000"},
        {c:"upbeat",min:0,max:21600000,ph:"1800000"}]},
  ]},
  {g:"Relay", rows:[
    {verb:"pulse", lab:"Pulse length",
     sub:"ms the relay stays closed \u2014 the length of the button press",
     f:[{c:"pulse",min:50,max:10000,ph:"500"}]},
    {verb:"presses", lab:"Presses per actuation",
     sub:"1-3, and ms between them. A blind retry \u2014 see the docs before raising it",
     f:[{c:"pcount",min:1,max:3,ph:"1"},{c:"pgap",min:200,max:5000,ph:"1000"}]},
    {verb:"gap", lab:"Interlock dead time",
     sub:"ms with both relays released before either fires. Protects the motor",
     f:[{c:"igap",min:100,max:5000,ph:"250"}]},
  ]},
];

/* Tabs.

   The selected tab is remembered per browser, because the person who opens
   Settings is usually in the middle of tuning something and will be back in a
   minute. Wrapped in try/catch: storage throws in a private window, and a
   dashboard that white-screens because it could not remember a tab would be a
   poor trade. */
function showTab(name){
  document.querySelectorAll(".tabs button").forEach(b=>{
    b.setAttribute("aria-selected", String(b.dataset.tab===name));
  });
  document.querySelectorAll(".tabpanel").forEach(p=>{
    p.hidden = (p.id !== "tab-"+name);
  });
  try{ localStorage.setItem("petdoor.tab", name); }catch(e){}
}

function initTabs(){
  document.querySelectorAll(".tabs button").forEach(b=>{
    b.addEventListener("click",()=>showTab(b.dataset.tab));
    // Left/right arrows move between tabs, which is what a tablist owes anyone
    // driving it from the keyboard.
    b.addEventListener("keydown",e=>{
      if(e.key!=="ArrowRight"&&e.key!=="ArrowLeft") return;
      const all=[...document.querySelectorAll(".tabs button")];
      const i=all.indexOf(b);
      const next=all[(i+(e.key==="ArrowRight"?1:all.length-1))%all.length];
      next.focus(); showTab(next.dataset.tab);
    });
  });
  let want="activity";
  try{ want = localStorage.getItem("petdoor.tab") || "activity"; }catch(e){}
  const btn=document.getElementById("tab-btn-"+want);
  if(!document.getElementById("tab-"+want) || (btn && btn.hidden)) want="activity";
  showTab(want);
}

let settingsRenderedFor=null;

function renderSettings(){
  const sec=$("settingssec"), host=$("settings");
  if(!sec||!host) return;
  // No web control means no settings to offer: hide the tab itself rather than
  // leaving a tab that opens onto nothing.
  // No web control means no settings to offer: hide that tab rather than
  // leaving one that opens onto nothing.
  const btn=$("tab-btn-settings");
  if(btn) btn.hidden=!controlOn;
  if(!controlOn){
    if(!$("tab-settings").hidden) showTab("activity");
    return;
  }

  const d=devices[0]||{};

  // Re-render ONLY when the door reports something different.
  //
  // This page reloads every 60 seconds, and a queued setting takes up to five
  // minutes to reach the door. Rebuilding the form on every poll would throw
  // away half-finished edits, and would snap the box you just changed back to
  // the old value — which reads as "it didn't work" and gets pressed again.
  // So the door's own report is the only thing that redraws these fields, and
  // until it arrives what you typed stays where you typed it.
  const sig=String(d.config||"")+"|"+String(d.device||"");
  if(settingsRenderedFor===sig && host.children.length) return;
  settingsRenderedFor=sig;

  const esc=t=>String(t==null?"":t).replace(/[<>&"]/g,"");
  const cfg=parseStatus(d.config);
  const known=Object.keys(cfg).length>0;

  let html="";
  if(!known){
    html+=`<div class="noconf"><b>The door has not reported its settings yet.</b>
      Either it has not called in since this server was updated, or it is running
      firmware from before it sent them. You can still change anything below \u2014
      the boxes just cannot show you what it is set to now.</div>`;
  }

  SETTINGS.forEach(grp=>{
    html+=`<div class="setgrp"><h3>${esc(grp.g)}</h3>`;
    grp.rows.forEach(r=>{
      const inputs=r.f.map(f=>{
        const cur=cfg[f.c];
        return `<input type="number" data-verb="${r.verb}" data-k="${f.c}"
          min="${f.min}" max="${f.max}" step="${f.step||1}"
          value="${cur!==undefined?esc(cur):""}" placeholder="${esc(f.ph)}"
          aria-label="${esc(r.lab)} ${esc(f.c)}">`;
      }).join("");
      html+=`<div class="srow"><div class="lab">${esc(r.lab)}<small>${esc(r.sub)}</small></div>`
        +inputs
        +`<button data-apply="${r.verb}">Apply</button></div>`;
    });
    html+=`</div>`;
  });

  // The buzzer is the one row that is not all numbers.
  const bpin=cfg.bpin, bp=cfg.bpassive, bl=cfg.blow;
  html+=`<div class="setgrp"><h3>Buzzer</h3><div class="srow">`
    +`<div class="lab">Annunciator<small>GPIO pin, or -1 for none. Ticks while the door moves, chimes when the travel time is up</small></div>`
    +`<input type="number" id="bz-pin" min="-1" max="48" step="1" placeholder="-1"
        value="${bpin!==undefined?esc(bpin):""}" aria-label="Buzzer GPIO pin">`
    +`<select id="bz-type" aria-label="Buzzer type">
        <option value="active"${bp==="0"?" selected":""}>active</option>
        <option value="passive"${bp==="1"?" selected":""}>passive</option>
      </select>`
    +`<select id="bz-pol" aria-label="Buzzer polarity">
        <option value="high"${bl==="0"?" selected":""}>active high</option>
        <option value="low"${bl==="1"?" selected":""}>active low</option>
      </select>`
    +`<button data-apply="buzzer">Apply</button></div></div>`;

  // The limit switches. Ships disabled (-1/-1), so this row is how they get
  // turned on once they are wired — no reflash, which is the whole point of
  // having it here before the hardware exists.
  const sOpen=cfg.sopen, sShut=cfg.sshut, sLow=cfg.slow;
  const fitted=(sOpen!==undefined&&+sOpen>=0)||(sShut!==undefined&&+sShut>=0);
  html+=`<div class="setgrp"><h3>Limit switches</h3><div class="srow">`
    +`<div class="lab">Position sensors<small>GPIO for the OPEN end and the CLOSED end, or -1 for an end with no switch. `
    +`Until one is fitted the door is open loop and only knows what it commanded.</small></div>`
    +`<input type="number" id="sn-open" min="-1" max="48" step="1" placeholder="-1"
        value="${sOpen!==undefined?esc(sOpen):""}" aria-label="Open limit switch GPIO">`
    +`<input type="number" id="sn-shut" min="-1" max="48" step="1" placeholder="-1"
        value="${sShut!==undefined?esc(sShut):""}" aria-label="Closed limit switch GPIO">`
    +`<select id="sn-pol" aria-label="Switch polarity">
        <option value="low"${sLow!=="0"?" selected":""}>to GND (usual)</option>
        <option value="high"${sLow==="0"?" selected":""}>to 3V3</option>
      </select>`
    +`<button data-apply="sensors">Apply</button></div>`
    +(fitted?"":`<p class="note" data-u="mt0">Not fitted yet. Wire a reed switch from each `
      +`GPIO to GND, set the pins here, and the door starts measuring its own position \u2014 `
      +`the arrival chime stops being a stopwatch and a swallowed button press becomes visible.</p>`)
    +`</div>`;

  // Things that lose state or take the door off the air. Each asks first.
  html+=`<div class="setgrp"><h3>Maintenance</h3><div class="ctl">`
    +`<button data-cmd="scan">Upload beacon scan</button>`
    +`<button data-cmd="resetstats">Reset statistics</button>`
    +`<button data-cmd="ota" data-confirm="Open an OTA window? You then have a few minutes to push firmware from a laptop.">Open OTA window</button>`
    +`<button data-cmd="reboot" data-confirm="Restart the door? It will be off the air for about a minute.">Restart door</button>`
    +`<button class="act-lock" data-cmd="defaults" data-confirm="Revert EVERY stored setting to the compiled-in defaults? Thresholds, dwell, filters, pulse and travel are all lost. The beacon list, the lock and the buzzer pin are kept.">Revert all settings</button>`
    +`</div></div>`;

  html+=`<div class="said" id="setsaid"></div>`;
  host.innerHTML=html;

  host.querySelectorAll("button[data-apply]").forEach(el=>{
    el.addEventListener("click",()=>applySetting(el.dataset.apply,host));
  });
  host.querySelectorAll("button[data-cmd]").forEach(el=>{
    el.addEventListener("click",()=>{
      const q=el.dataset.confirm;
      if(q && !confirm(q)) return;
      send(el.dataset.cmd,"setsaid",!!q);
    });
  });
}

/* Builds "<verb> <v1> <v2>" from the row's inputs and queues it. Empty boxes
   are refused here rather than sent as a blank the door would reject. */
function applySetting(verb,host){
  const say=$("setsaid");
  let cmd;
  if(verb==="sensors"){
    const o=$("sn-open").value.trim(), c=$("sn-shut").value.trim();
    if(o===""||c===""){say.className="said bad";say.textContent="Give both pins — use -1 for an end with no switch.";return;}
    cmd = (+o < 0 && +c < 0) ? "sensors off" : `sensors ${o} ${c} ${$("sn-pol").value}`;
  }else if(verb==="buzzer"){
    const pin=$("bz-pin").value.trim();
    if(pin==="") {say.className="said bad";say.textContent="Give the buzzer a GPIO pin, or -1 for none.";return;}
    cmd = (+pin < 0) ? "buzzer off"
        : `buzzer ${pin} ${$("bz-type").value} ${$("bz-pol").value}`;
  }else{
    const vals=[...host.querySelectorAll(`input[data-verb="${verb}"]`)].map(i=>i.value.trim());
    if(vals.some(v=>v==="")){
      say.className="said bad";
      say.textContent="Fill in every box on that row first.";
      return;
    }
    cmd=verb+" "+vals.join(" ");
  }
  send(cmd,"setsaid",false);
}

/* The control panel.

   Hidden unless the server was started with --allow-web-control, so a
   deployment that has not made that decision shows no buttons at all rather
   than buttons that 403.

   The honesty problem this panel has to solve: nothing here happens when you
   press it. The door collects commands on its next check-in, which is
   opportunistic and gated on the beacon being away, so "now" means "within
   about five minutes". A button that looked like it had opened the door would
   be lying, and the person would press it again. So every press moves the
   command into a visible WAITING list, and the list stays until the door has
   actually taken it. */
/* Windows arrive in the door's config line as HHMM-HHMM/<day mask in hex>,
   comma separated, or "-" for none. The door is the only thing that knows what
   it has stored, so everything here is a view of what it reported rather than
   of what we last sent it. */
const DAYNAME=["Sun","Mon","Tue","Wed","Thu","Fri","Sat"];

function parseWindows(spec){
  if(!spec||spec==="-") return [];
  return spec.split(",").map(t=>{
    const m=/^(\d{2})(\d{2})-(\d{2})(\d{2})\/([0-9a-fA-F]{1,2})$/.exec(t.trim());
    if(!m) return null;
    return {s:m[1]+":"+m[2], e:m[3]+":"+m[4], mask:parseInt(m[5],16)};
  }).filter(Boolean);
}

function daysLabel(mask){
  if((mask&0x7F)===0x7F) return "every day";
  return DAYNAME.filter((_,i)=>mask&(1<<i)).join(" ");
}

function renderSchedule(){
  const sec=$("schedsec"), host=$("schedule");
  if(!sec||!host) return;
  if(!controlOn){ sec.hidden=true; return; }
  // Local, like every other renderer here. It was global-by-accident in the
  // first version of this panel, which threw "Can't find variable: esc" and
  // took the whole dashboard down — a parse check cannot catch that.
  const esc=t=>String(t==null?"":t).replace(/[<>&"]/g,"");
  sec.hidden=false;

  const d=devices[0]||{};
  const cfg=parseStatus(d.config);
  const known=Object.keys(cfg).length>0;
  const wins=parseWindows(cfg.sched);
  const tz=cfg.tz!==undefined?Number(cfg.tz):null;

  let html="";
  if(!known||cfg.sched===undefined){
    html+=`<div class="noconf"><b>The door has not reported a schedule yet.</b>
      Either it has not called in since this server was updated, or it is running
      firmware from before scheduled lockout existed. Anything you add below is
      still queued for it.</div>`;
  }

  if(wins.length){
    wins.forEach((w,i)=>{
      const overnight=w.s>w.e;
      html+=`<div class="schedrow">
        <span class="schedwin">${esc(w.s)} \u2013 ${esc(w.e)}</span>
        <span class="scheddays">${esc(daysLabel(w.mask))}${overnight?" \u00b7 overnight":""}</span>
        <span data-u="flex1"></span>
        <button class="b-quiet" data-schedel="${esc(w.s)}-${esc(w.e)}">Remove</button></div>`;
    });
  } else if(known&&cfg.sched!==undefined){
    html+=`<p class="note">No windows. The collar may open the door at any hour.</p>`;
  }

  const dayBoxes=DAYNAME.map((n,i)=>
    `<label><input type="checkbox" class="schedday" value="${i}" checked>${n}</label>`).join("");

  // Nothing here is instant, and a panel that looks unchanged after you press
  // Remove reads as a broken button rather than a queued command. The door
  // collects on its next check-in, and only reports the result on the one
  // after that, so a change can take ten minutes to appear.
  const waiting=(typeof pending!=="undefined"?pending:[])
    .filter(p=>String(p.command||"").toLowerCase().startsWith("schedule"));
  if(waiting.length){
    html+=`<div class="queued" data-u="mt12">
      <span>Waiting for the door:</span>
      <span class="cmds">${waiting.map(p=>esc(p.command)).join(", ")}</span>
      <p class="note" data-u="m-note">The list above still shows what the door
      currently has. It collects queued changes when it next calls in — usually within
      five minutes — and this panel updates after the check-in following that.</p></div>`;
  }

  html+=`<div class="schedadd">
      <label for="schedfrom">From</label><input type="time" id="schedfrom" value="22:00">
      <label for="schedto">to</label><input type="time" id="schedto" value="06:00">
      <span class="daypick">${dayBoxes}</span>
      <button class="b-outline" id="schedadd">Add window</button>
    </div>`;

  html+=`<div class="schedadd" data-u="mt14">
      <label for="schedtz">Local time is UTC</label>
      <input type="number" id="schedtz" min="-840" max="840" step="15"
             data-u="w7" value="${tz!==null?tz:0}">
      <span class="scheddays">minutes</span>
      <button class="b-quiet" id="schedtzset">Set</button>
      ${wins.length?`<span data-u="flex1"></span>
        <button class="b-danger" id="schedclear">Remove all</button>`:""}
    </div>`;

  // Said here rather than only in the docs. These are the two things that
  // surprise people, and both of them surprise people at night.
  html+=`<p class="note"><b>A window stops the collar opening the door.</b> It never
     stops the door closing, and it cannot let an animal back in \u2014 so count them
     in before one starts, the same as you would if you were bolting the door.<br>
     Windows are inert until the door has a real clock from the network, and there
     is no daylight-saving handling: the offset above is a fixed number of minutes,
     so leave slack at both ends.</p>`;

  host.innerHTML=html;

  // Deletes name the window by its TIMES, never by its position.
  //
  // Position was the obvious choice and it was wrong twice over. Removing a
  // window renumbers the rest, so clicking Remove on two of them deleted the
  // first and then failed on the second — which, over the remote channel, is
  // heard only as a refusal tone. And an index from a page that had gone stale
  // pointed at whichever window had shifted into that slot, so the failure
  // mode was deleting the WRONG window silently.
  //
  // Times are order-independent and either match or fail cleanly. Needs
  // firmware that accepts `schedule del HH:MM-HH:MM`; older builds take only
  // a number and will refuse this with "no window".
  host.querySelectorAll("button[data-schedel]").forEach(el=>{
    el.addEventListener("click",()=>send("schedule del "+el.dataset.schedel,null,true));
  });
  const addBtn=$("schedadd");
  if(addBtn) addBtn.addEventListener("click",()=>{
    const f=$("schedfrom").value, t=$("schedto").value;
    if(!f||!t) return;
    const picked=[...host.querySelectorAll(".schedday")].filter(c=>c.checked)
                   .map(c=>DAYNAME[Number(c.value)]);
    if(!picked.length){ alert("Pick at least one day."); return; }
    const days=picked.length===7?"daily":picked.join(",");
    send(`schedule add ${f}-${t} ${days}`,null,true);
  });
  const tzBtn=$("schedtzset");
  if(tzBtn) tzBtn.addEventListener("click",()=>send("schedule tz "+$("schedtz").value,null,true));
  const clr=$("schedclear");
  if(clr) clr.addEventListener("click",()=>{
    if(confirm("Remove every lockout window? The collar will be able to open the door at any hour."))
      send("schedule clear",null,true);
  });
}

function renderControl(){
  const sec=$("controlsec"), host=$("controls");
  if(!sec||!host) return;
  sec.hidden=!controlOn;
  // The Controls tab still has a use with web control off — the log of what was
  // queued from the command line is worth reading either way — so the tab stays
  // and only the buttons go.
  if(!controlOn) return;

  const esc=t=>String(t==null?"":t).replace(/[<>&]/g,"");
  const d=devices[0]||{};
  const st=parseStatus(d.status);
  const locked=st.locked==="1";
  // Seconds left in a maintenance window; absent on firmware older than this
  // field, which reads as 0 and simply offers to start one.
  const maintLeft=Number(st.maint||0)||0;
  const holdOn=st.hold==="1"||st.hold==="2";
  const stand=doorStanding(d);

  // Disabled rather than hidden when it would be a no-op: a button that
  // vanishes is confusing, one that is visibly unavailable explains itself.
  const b=(cmd,label,cls,off)=>`<button class="${cls}" data-cmd="${cmd}"${off?" disabled":""}>${label}</button>`;
  const group=(label,inner)=>`<div class="ctlgroup"><span class="glabel">${label}</span><div class="ctl">${inner}</div></div>`;

  // Grouped by what each acts on, and weighted by what you came to do. Open is
  // what people reach for from the garden, so it is the only solid button on
  // the page; Close is the other real actuation; the rest move nothing and
  // recede. Six equally-coloured buttons say "pick one of these six".
  const buttons='<div class="ctlgroups">'
    +group("Door",
        b("door open","Open","b-primary",false)
       +b("door close","Close","b-outline",false)
       +b("door auto","Auto","b-quiet",false))
    +group("Collar access",
        b("lock","Lock","b-danger",locked)
       +b("unlock","Unlock","b-quiet",!locked))
    // Separate from "Collar access" on purpose: that stops the collar opening
    // the door, this stops the door deciding anything at all. Both confirm,
    // and held-closed confirms in the strongest terms the dialog allows,
    // because nothing about it expires.
    +group("Hold position",
        `<button class="b-outline" data-cmd="lock open" data-confirm="Hold the door OPEN? It will open now and nothing automatic will close it again — not the collar, not the close dwell, not the schedule. This SURVIVES A REBOOT and does not expire. Release it with Unlock.">Hold open</button>`
       +`<button class="act-lock" data-cmd="lock close" data-confirm="Hold the door CLOSED? An animal outside WILL NOT BE ABLE TO GET IN, and nothing will correct that — this does not expire and survives a reboot and a power cut. Only you can release it, with Unlock. Are you sure?">Hold closed</button>`
       +(holdOn?`<button class="b-quiet" data-cmd="unlock">Release hold</button>`:""))
    +group("Check",
        b("beep","Beep","b-quiet",false))
    // Its own group because it is not an action on the door so much as a
    // change to what the door is: for the window's duration the collar stops
    // working. Saying how long it lasts on the button itself matters — the
    // safety property here is that it ends by itself, and a control that does
    // not say so invites somebody to leave it on.
    +group("Maintenance",
        (maintLeft>0
          ? `<button class="b-danger" data-cmd="maint off">End maintenance (${Math.ceil(maintLeft/60)} min left)</button>`
            // Where to point a console. Shown only while a window is open,
            // because that is the only time anything is listening there.
            +(st.ip&&st.ip!=="-"?`<span class="glabel" data-u="self-c">console: ${esc(st.ip)}:23</span>`:"")
            // One button per duration rather than a free-text box, on purpose.
            // The firmware takes any number of minutes up to MAINT_MAX_MS (4 h),
            // but the comment above is the reason not to offer a bare field: the
            // safety property is that the window ENDS BY ITSELF, and a control
            // that does not state its own duration invites leaving it on. A
            // button that says "4 h" cannot be misread; a box showing "240"
            // can.
          : MAINT_CHOICES.map(m=>
              `<button class="b-quiet" data-cmd="maint ${m}"`
              +` data-confirm="Start a ${maintLabel(m)} maintenance window? The collar will NOT open or close the door until it expires, and the console opens over WiFi. The window ends by itself.">`
              +`${maintLabel(m)}</button>`).join("")))
    +'</div>';

  let queue="";
  if(pending.length){
    queue=`<div class="queued">`
      +`<span>Waiting for the door:</span> <span class="cmds">${pending.map(p=>esc(p.command)).join(", ")}</span>`
      +`<button id="cancelq" data-dev="${esc(d.device||"")}">Cancel</button></div>`;

    // Queueing `door close` on a door that already reports CLOSED looks like a
    // mistake and is not one: manual commands call forcePulse*(), which
    // bypasses the "already in that state" check on purpose. The door is open
    // loop, so when its belief is wrong — a shove by hand, a press the
    // controller swallowed — pressing again is exactly how you correct it.
    // Saying so beats leaving somebody to wonder whether it registered.
    const here=(stand.state||"").toUpperCase();
    const redundant=pending.filter(p=>{
      const c=String(p.command||"").toLowerCase();
      return (c==="door close"&&here==="CLOSED")||(c==="door open"&&here==="OPEN");
    });
    if(redundant.length){
      queue+=`<p class="note" data-u="mt8">The door already reports
        <b>${esc(here)}</b>. This still pulses the relay — manual commands skip the
        "already in that state" check, which is how you correct the door when its
        idea of where it is has drifted from reality.</p>`;
    }
  }

  host.innerHTML=buttons+queue
    +`<p class="note"><b>Open</b> and <b>Close</b> press the door controller's buttons once. `
    +`<b>Auto</b> hands control back to the collar. <b>Lock</b> stops the collar opening the door `
    +`at all — it does not stop an open door closing.<br>`
    +`Nothing here is instant: the door collects commands when it next calls in, usually within `
    +`five minutes, and sooner if the collar is away. Until then you can cancel.</p>`
    +`<div class="said" id="said"></div>`;

  host.querySelectorAll("button[data-cmd]").forEach(el=>{
    el.addEventListener("click",()=>send(el.dataset.cmd));
  });
  const c=$("cancelq");
  if(c) c.addEventListener("click",cancelQueued);
}

/* The custom header is the CSRF defence: a browser will not attach it to a
   cross-origin request without a preflight this server never answers, so a
   malicious page cannot ride your dashboard session to open the door. */
async function send(cmd,sayId,confirmed){
  if(busy) return;
  busy=true;
  const say=$(sayId||"said");
  if(say){say.className="said";say.textContent="Sending "+cmd+"…";}
  try{
    const r=await fetch("/api/command",{
      method:"POST",
      headers:{"Content-Type":"application/json","X-PetDoor-Control":"1"},
      body:JSON.stringify(confirmed?{command:cmd,confirm:true}:{command:cmd})
    });
    const j=await r.json().catch(()=>({ok:false,error:"HTTP "+r.status}));
    if(say){
      say.className="said "+(j.ok?"good":"bad");
      say.textContent=j.ok?(j.message||"queued"):("Refused: "+(j.error||"unknown"));
    }
  }catch(err){
    if(say){say.className="said bad";say.textContent="Could not reach the server ("+err.message+")";}
  }finally{
    busy=false;
    // Start watching closely: the door is about to do something, and the whole
    // point of pressing the button was to see it happen.
    if(typeof watchClosely==="function") watchClosely();
    load();
  }
}

async function cancelQueued(){
  if(busy) return;
  busy=true;
  try{
    const dev=$("cancelq")?.dataset.dev||"";
    await fetch("/api/command"+(dev?"?device="+encodeURIComponent(dev):""),
      {method:"DELETE",headers:{"X-PetDoor-Control":"1"}});
  }catch(err){/* load() below surfaces the real state either way */}
  busy=false;
  if(typeof watchClosely==="function") watchClosely();
  load();
}

/* Two logs, split by what the command DID.

   Asking the door to open is a different kind of event from retuning its exit
   threshold: one is a thing you did to the door today, the other changes how it
   behaves from now on. They were one table while there was one page; with a tab
   each they belong beside the panel that produced them. */
const CONTROL_VERBS=new Set(["door","lock","unlock","beep","scan","resetstats","reboot","ota"]);

// withMoves: also list the door moving when nothing here asked it to — a hand on
// the flap, the controller's own panel or timer. Without them the Controls log
// reads as a complete record of the door's movements and is not one: the door
// logged and uploaded both, and this page simply had nowhere to put them.
function renderLog(hostId,keep,empty,withMoves){
  const host=$(hostId);
  if(!host) return;
  const esc=t=>String(t==null?"":t).replace(/[<>&]/g,"");
  const rows=commands.filter(c=>c.delivered)
    .filter(c=>keep(String(c.command||"").split(" ")[0].toLowerCase()))
    .map(c=>({t:c.delivered||c.queued,cmd:c}));
  if(withMoves)
    for(const e of events) if(e.type==="UNCOMMANDED"&&e.epoch) rows.push({t:e.epoch,ev:e});
  rows.sort((a,b)=>b.t-a.t);
  if(!rows.length){ host.innerHTML=`<div class="empty">${empty}</div>`; return; }
  const stamp=t=>{const w=new Date(t*1000);return `${fmtDate(t)} ${pad(w.getHours())}:${pad(w.getMinutes())}`;};
  host.innerHTML='<table><thead><tr><th>When</th><th>Command</th><th>Result</th></tr></thead><tbody>'
    +rows.slice(0,20).map(r=>{
      if(r.ev) return `<tr><td class="mono">${stamp(r.t)}</td>`
        +`<td data-u="c-ink2">not from here</td><td>${uncommandedDetail(r.ev)}</td></tr>`;
      const c=r.cmd;
      const ok=c.ack && !/refus|reject|unknown/i.test(c.ack);
      return `<tr><td class="mono">${stamp(r.t)}</td>`
        +`<td class="mono">${esc(c.command)}</td>`
        +`<td class="${c.ack?(ok?"":"warn"):"dim"}">${c.ack?esc(c.ack):"awaiting the door's next upload"}</td></tr>`;
    }).join("")+"</tbody></table>";
}

function renderChanges(){
  renderLog("ctllog", v=>CONTROL_VERBS.has(v),  "Nothing has been asked of the door yet.", true);
  renderLog("setlog", v=>!CONTROL_VERBS.has(v), "No settings have been changed remotely.");
}

/* The three facts worth having beside the name: what it is running, how much it
   has told us, and whether it is still talking to us. */
function renderBanner(){
  const host=$("bmeta");
  if(!host) return;
  const esc=t=>String(t==null?"":t).replace(/[<>&]/g,"");
  const d=devices[0]||{};
  const fact=(k,v,cls)=>`<div class="fact ${cls||""}"><span class="k">${k}</span><span class="v">${v}</span></div>`;
  const st=parseStatus(d.status);
  const bits=[];
  // The door's own name leads: with several doors it is the thing that tells
  // you which one everything else on this page is describing.
  if(d.device) bits.push(fact("Door",esc(d.device)));
  if(d.version) bits.push(fact("Firmware","v"+esc(d.version)));
  bits.push(fact("Events",events.length.toLocaleString()));
  if(st.rssi!==undefined){
    bits.push(fact("Signal",esc(st.rssi)+" dBm"+(st.dist?` <span data-u="fw4 c-ink3">~${esc(st.dist)} m</span>`:"")));
  }
  // Worst gap and boots are diagnostics, but they are the two that explain a
  // door misbehaving — a gap near the fix timeout, or a boot count climbing
  // between uploads, which is what a brownout looks like from here.
  if(st.gap) bits.push(fact("Worst gap",esc(st.gap)+" ms",
                            +st.gap>3000?"stale":""));
  if(d.boots!==undefined&&d.boots!==null) bits.push(fact("Boots",String(d.boots)));
  if(d.last_seen){
    const mins=Math.round((Date.now()/1000-d.last_seen)/60);
    const when=mins<1?"just now":mins<60?`${mins} min ago`:
      mins<2880?`${Math.round(mins/60)} h ago`:`${Math.round(mins/1440)} days ago`;
    // The only fact here allowed to change colour, so that when it does it is
    // the one thing the eye lands on. Three hours without a word is not a
    // quiet door, it is a door to go and look at.
    bits.push(fact("Last check-in",when,mins>180?"stale":""));
  }else{
    bits.push(fact("Last check-in","never","stale"));
  }
  bits.push(fact("Refreshed",'<span id="live">just now</span>'));
  // Hidden on a phone: it is an OTA-time detail, and nobody pushes firmware
  // from one. Dropping it saves a whole wrapped row where space is tightest.
  if(d.door_ip) bits.push(fact("Push to",`<span class="mono" data-u="fs12">${esc(d.door_ip)}</span>`,"wide-only"));
  host.innerHTML=bits.join("");

  // The live state rides in the title row itself. It is the one thing someone
  // opens this page to see, so it should not be below a card heading.
  const hs=$("hstate");
  if(hs){
    if(!d.device){ hs.innerHTML=""; return; }
    const stand=doorStanding(d);
    const present=st.present==="1";
    hs.innerHTML=`<span class="pill ${present?"in":"out"}">${present?"beacon present":"beacon away"}</span>`
      +`<span class="pill ${stand.state==="OPEN"?"in":""}">door ${esc(stand.state||st.door||"?")}`
      +(stand.label?` <span data-u="fw4 dim">${esc(stand.label)}</span>`:"")
      +`</span>`
      +(st.locked==="1"?`<span class="pill lock">LOCKED</span>`:"");
  }
}

function renderAll(){renderStats();renderStrip();renderDurations();renderPerDay();
  renderHourly();renderSignal();renderTable();renderChanges();renderBanner();
}




/* ---- data comes from the log server ---- */
let lastLoad=0;
async function load(){
  try{
    const r=await fetch("/api/events",{cache:"no-store"});
    if(!r.ok) throw new Error("HTTP "+r.status);
    const j=await r.json();
    events=(j.events||[]).map(e=>({epoch:+e.epoch||0,uptime:+e.uptime||0,boot:+e.boot||0,
      type:e.type,detail:+e.detail||0,rssi:+e.rssi||0,src:+e.src||0,device:e.device||""}));
    events.sort((a,b)=>(a.epoch||0)-(b.epoch||0)||a.boot-b.boot||a.uptime-b.uptime);
    commands=(j.commands||[]);
    pending=(j.pending||[]);
    controlOn=!!j.control;
    devices=(j.devices||[]);
    renderDoors(devices);
    renderControl();
    renderSchedule();
    renderSettings();
    lastLoad=Date.now();
    renderAll();
    // renderAll() rebuilt the banner, so #live exists again from here.
    const devs=[...new Set(events.map(e=>e.device).filter(Boolean))];
    if($("live")){
      // Say when the page is watching closely, so a five-second poll is a
      // visible state rather than unexplained network traffic.
      const watching = typeof inFlight==="function" && inFlight() &&
                       typeof fastUntil!=="undefined" && Date.now()<fastUntil;
      $("live").textContent = watching ? "watching \u00b7 every 5s"
                            : events.length ? "just now" : "no events";
    }
  }catch(err){
    // The banner is the one place a failure cannot be scrolled past.
    const host=$("bmeta");
    if(host) host.innerHTML='<span class="stale">cannot reach the log server</span>'
      +'<span class="quiet">'+String(err.message).replace(/[<>&]/g,"")+'</span>';
  }finally{
    // Pick the next interval from the data we just fetched, not the data we
    // had before it. Every path into load() gets the right pace this way:
    // the poll loop, the Refresh button, and a button press.
    if(typeof scheduleNextLoad==="function") scheduleNextLoad();
  }
}
$("refresh").onclick=load;
initTabs();
renderAll();

/* Poll fast only while something is actually in flight.

   Once a minute is right for a wall display: the door uploads in bursts and
   there is nothing to see between them. But it is useless in the minute after
   you press Open — the thing you are standing there watching for arrives, and
   the page sits on the old state until the next tick.

   So: 5 seconds while a command is outstanding, 60 otherwise. "Outstanding"
   means queued and not yet collected, or collected and not yet acknowledged —
   the two windows where the door's state is about to change and the page does
   not know it yet.

   Bounded, because a door that never calls in must not leave a browser tab
   polling every 5 seconds until the heat death of the laptop battery. After
   FAST_FOR_MS the pace drops back regardless, and the pending list still says
   why nothing has happened. */
const FAST_MS=5000, SLOW_MS=60000, FAST_FOR_MS=15*60*1000;
let fastUntil=0, loading=false, pollTimer=null;

function inFlight(){
  if(pending.length) return true;
  // Delivered but not yet acknowledged: the door has it and the result is on
  // its way. With the forced-flush fix that is usually seconds.
  return commands.some(c=>c.delivered && !c.ack);
}

function scheduleNextLoad(){
  clearTimeout(pollTimer);
  const fast = inFlight() && Date.now() < fastUntil;
  pollTimer = setTimeout(pollOnce, fast ? FAST_MS : SLOW_MS);
}

async function pollOnce(){
  if(loading){ scheduleNextLoad(); return; }   // never stack requests
  loading = true;
  try { await load(); } finally { loading = false; }
  // load() reschedules; see below for why it and not this.
}

// Called whenever we do something that will change the door's state, so the
// page starts watching closely from that moment rather than on the next tick.
//
// This only opens the WINDOW. The rate is chosen at the end of load(), because
// that is the only place that knows whether anything is actually in flight —
// scheduling here used the pending list from BEFORE the command was queued,
// saw nothing outstanding, and quietly picked the one-minute tick. The label
// said "watching every 5s" while the page polled once a minute.
function watchClosely(){
  fastUntil = Date.now() + FAST_FOR_MS;
}

pollOnce();
setInterval(()=>{
  if(!lastLoad || !$("live")) return;
  // Leave the "watching" label alone; it is describing something live.
  if(inFlight() && Date.now()<fastUntil) return;
  const m=Math.round((Date.now()-lastLoad)/60000);
  if(m>=1&&events.length) $("live").textContent=`${m} min ago`;
},60000);
