// Every event the door can log, labelled as the server's /table labels it
// (petdoor-logserver.py EVENT_LABEL). An event missing here used to render as
// "undefined" — which is how a door moved by hand vanished from this page while
// sitting correctly in the database.
const EV={OPEN:"came in",CLOSE:"went out",BOOT:"restarted",REFUSED:"refused",FIX_GOT:"beacon found",FIX_LOST:"beacon lost",STALLED:"did not complete its travel",
  UNCOMMANDED:"moved, not by PetDoor",RETRY:"pressed again",GAVE_UP:"gave up closing",WAKE:"woke the controller",NO_MOVE:"did not move at all",
  MAINT:"maintenance mode",MANUAL:"under manual control",CONSOLE:"network console",BEACON_LOW:"beacon battery",SENSOR_FAULT:"sensor fault",SENSOR_OK:"sensor recovered"};
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
// OPEN and CLOSE spend `detail` on the ActuationSource (petdoor/door.h). Kept in
// step with dashboard.js, where a missing branch left the Detail column blank
// for every door movement and made a fail-safe reversal look like a plain open.
const SRC={0:"beacon",1:"console",2:"network",3:"fail-safe reversal"};
let events=[], commands=[], selected=null;

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
     if(e.type==="BOOT"&&e.detail===9) d=`<strong data-u="c-fault">${d}</strong>`;
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

function renderDoors(devs){
  const host=$("doors");
  if(!devs.length){host.innerHTML="";return;}
  const now=Date.now()/1000;
  host.innerHTML='<div class="doors">'+devs.map(d=>{
    const esc=t=>String(t==null?"":t).replace(/[<>&]/g,"");
    const mins=Math.round((now-(d.last_seen||0))/60);
    // A door that has not called in for hours is either off, off the network,
    // or failing to upload — worth flagging rather than leaving to be noticed.
    const stale=mins>180;
    const when=mins<1?"just now":mins<60?`${mins} min ago`:`${Math.round(mins/60)} h ago`;
    // Reboots climbing between uploads is the signature of a power problem.
    const boots=d.boots||0;
    const st=parseStatus(d.status);

    let rows=`<div class="n">${esc(d.device||"door")}</div>`
      +`<div class="m">firmware v${esc(d.version||"?")} &middot; ${boots} boot${boots===1?"":"s"}</div>`
      +`<div class="m${stale?" warn":""}">last heard ${when}${stale?" — check it":""}</div>`;

    // What it can see right now. This is the number you tune against, and it
    // used to exist only on a serial console nobody can reach.
    if(st.rssi!==undefined){
      const present=st.present==="1";
      rows+=`<div class="live">`
        +`<span class="pill ${present?"in":"out"}">${present?"beacon present":"beacon away"}</span>`
        +`<span class="pill ${st.door==="OPEN"?"in":""}">door ${esc(st.door||"?")}</span>`
        // A locked door will not open for the collar. That is the one state
        // worth shouting about, because from the outside it looks identical to
        // a door that is simply shut.
        +(st.locked==="1"?`<span class="pill lock">LOCKED — collar cannot open it</span>`:"")
        +`</div>`
        +`<div class="m">${esc(st.rssi)} dBm`
        +(st.dist?` &middot; ~${esc(st.dist)} m`:"")
        +(st.gap?` &middot; worst gap ${esc(st.gap)} ms`:"")+`</div>`;
    }

    // Health, and only when there is something to say about it.
    const warns=[];
    if(st.heap!==undefined && +st.heap<30000) warns.push(`low memory (${st.heap} bytes free)`);
    if(st.gap!==undefined && +st.gap>3000) warns.push(`worst gap ${st.gap} ms exceeds the 3 s fix timeout`);
    if(warns.length) rows+=`<div class="m warn">${warns.map(esc).join(" &middot; ")}</div>`;

    // Where to push an update, and whether this build would even take one.
    if(d.door_ip) rows+=`<div class="m mono">push to ${esc(d.door_ip)}</div>`;
    if(d.remote===0) rows+=`<div class="m warn">firmware predates remote commands — anything queued will wait</div>`;

    return `<div>${rows}</div>`;
  }).join("")+"</div>";
}

/* Config changes, newest first, with what the door made of each. Shown beside
   the behaviour they altered so the two can be read together. */
function renderChanges(){
  const host=$("changes");
  if(!host) return;
  const applied=commands.filter(c=>c.delivered);
  if(!applied.length){
    host.innerHTML='<div class="empty">No settings have been changed remotely.</div>';
    return;
  }
  const esc=t=>String(t==null?"":t).replace(/[<>&]/g,"");
  host.innerHTML='<table><thead><tr><th>When</th><th>Change</th><th>Result</th></tr></thead><tbody>'
    +applied.slice(0,20).map(c=>{
      const when=new Date((c.delivered||c.queued)*1000);
      const ok=c.ack && !/refus|reject|unknown/i.test(c.ack);
      return `<tr><td class="mono">${fmtDate(c.delivered||c.queued)} ${pad(when.getHours())}:${pad(when.getMinutes())}</td>`
        +`<td class="mono">${esc(c.command)}</td>`
        +`<td class="${c.ack?(ok?"":"warn"):"dim"}">${c.ack?esc(c.ack):"awaiting the door's next upload"}</td></tr>`;
    }).join("")+"</tbody></table>";
}

function renderAll(){renderStats();renderStrip();renderDurations();renderPerDay();
  renderHourly();renderSignal();renderTable();renderChanges();
  const dated=events.filter(e=>e.epoch>0);
  $("sub").textContent=events.length?`${events.length} events`+(dated.length?` · ${fmtDate(dated[0].epoch)} to ${fmtDate(dated[dated.length-1].epoch)}`:" · no clock set on the door"):"Comings and goings, from the PetDoor event log";
}




window.__PETDOOR_SAMPLE__={"events":[{"device":"petdoor-sample","epoch":1788091200,"uptime":410,"boot":1,"type":"BOOT","detail":1,"rssi":0},{"device":"petdoor-sample","epoch":1788099298,"uptime":817,"boot":1,"type":"OPEN","detail":0,"rssi":-57},{"device":"petdoor-sample","epoch":1788099361,"uptime":1637,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788099584,"uptime":1707,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788099667,"uptime":1947,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1788113643,"uptime":2395,"boot":1,"type":"OPEN","detail":0,"rssi":-57},{"device":"petdoor-sample","epoch":1788113670,"uptime":2770,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1788114535,"uptime":3042,"boot":1,"type":"OPEN","detail":0,"rssi":-47},{"device":"petdoor-sample","epoch":1788114573,"uptime":3250,"boot":1,"type":"CLOSE","detail":0,"rssi":-70},{"device":"petdoor-sample","epoch":1788128158,"uptime":4082,"boot":1,"type":"OPEN","detail":0,"rssi":-51},{"device":"petdoor-sample","epoch":1788128223,"uptime":4664,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1788128531,"uptime":5507,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1788128618,"uptime":5694,"boot":1,"type":"CLOSE","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1788132901,"uptime":6205,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1788132971,"uptime":6433,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1788133845,"uptime":6862,"boot":1,"type":"OPEN","detail":0,"rssi":-44},{"device":"petdoor-sample","epoch":1788133899,"uptime":7101,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788144016,"uptime":7729,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1788144059,"uptime":8227,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788144274,"uptime":9024,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1788144349,"uptime":9359,"boot":1,"type":"CLOSE","detail":0,"rssi":-82},{"device":"petdoor-sample","epoch":1788150436,"uptime":9862,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788150506,"uptime":10339,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788152128,"uptime":10860,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1788152207,"uptime":11521,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1788182923,"uptime":12236,"boot":1,"type":"OPEN","detail":0,"rssi":-43},{"device":"petdoor-sample","epoch":1788182959,"uptime":12441,"boot":1,"type":"CLOSE","detail":0,"rssi":-70},{"device":"petdoor-sample","epoch":1788183121,"uptime":12959,"boot":1,"type":"OPEN","detail":0,"rssi":-49},{"device":"petdoor-sample","epoch":1788183189,"uptime":13188,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1788196210,"uptime":13761,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788196242,"uptime":14173,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788196773,"uptime":14622,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1788196816,"uptime":14716,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1788209958,"uptime":14784,"boot":1,"type":"OPEN","detail":0,"rssi":-58},{"device":"petdoor-sample","epoch":1788209996,"uptime":15511,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1788212637,"uptime":15814,"boot":1,"type":"OPEN","detail":0,"rssi":-47},{"device":"petdoor-sample","epoch":1788212720,"uptime":16205,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1788229425,"uptime":16506,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1788229450,"uptime":17161,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1788231538,"uptime":17200,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788231602,"uptime":17751,"boot":1,"type":"CLOSE","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1788268679,"uptime":17957,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788268715,"uptime":18736,"boot":1,"type":"CLOSE","detail":0,"rssi":-70},{"device":"petdoor-sample","epoch":1788269758,"uptime":19024,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788269806,"uptime":19568,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1788282085,"uptime":19990,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788282140,"uptime":20590,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1788282990,"uptime":21275,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1788283025,"uptime":22027,"boot":1,"type":"CLOSE","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1788300150,"uptime":22269,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788300191,"uptime":22564,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788300565,"uptime":22689,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788300643,"uptime":23078,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1788311504,"uptime":23156,"boot":1,"type":"OPEN","detail":0,"rssi":-43},{"device":"petdoor-sample","epoch":1788311564,"uptime":23333,"boot":1,"type":"CLOSE","detail":0,"rssi":-70},{"device":"petdoor-sample","epoch":1788313611,"uptime":24098,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788313656,"uptime":24913,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1788320162,"uptime":25301,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-85},{"device":"petdoor-sample","epoch":1788320164,"uptime":25461,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-70},{"device":"petdoor-sample","epoch":1788324541,"uptime":26265,"boot":1,"type":"OPEN","detail":0,"rssi":-51},{"device":"petdoor-sample","epoch":1788324569,"uptime":26531,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1788325099,"uptime":27291,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788325157,"uptime":28170,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788326666,"uptime":28312,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-93},{"device":"petdoor-sample","epoch":1788326669,"uptime":28352,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-84},{"device":"petdoor-sample","epoch":1788352043,"uptime":28701,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-90},{"device":"petdoor-sample","epoch":1788352046,"uptime":29579,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788368613,"uptime":29992,"boot":1,"type":"OPEN","detail":0,"rssi":-44},{"device":"petdoor-sample","epoch":1788368672,"uptime":30472,"boot":1,"type":"CLOSE","detail":0,"rssi":-75},{"device":"petdoor-sample","epoch":1788368851,"uptime":30577,"boot":1,"type":"OPEN","detail":0,"rssi":-45},{"device":"petdoor-sample","epoch":1788368927,"uptime":31472,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1788375466,"uptime":31882,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-88},{"device":"petdoor-sample","epoch":1788375469,"uptime":32235,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1788377070,"uptime":32517,"boot":1,"type":"OPEN","detail":0,"rssi":-43},{"device":"petdoor-sample","epoch":1788377099,"uptime":33326,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788379710,"uptime":33877,"boot":1,"type":"OPEN","detail":0,"rssi":-49},{"device":"petdoor-sample","epoch":1788379776,"uptime":34582,"boot":1,"type":"CLOSE","detail":0,"rssi":-82},{"device":"petdoor-sample","epoch":1788384457,"uptime":34658,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1788384487,"uptime":34987,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1788385529,"uptime":35410,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788385588,"uptime":36052,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1788399348,"uptime":36316,"boot":1,"type":"OPEN","detail":0,"rssi":-44},{"device":"petdoor-sample","epoch":1788399407,"uptime":36836,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1788402491,"uptime":36897,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788402559,"uptime":37109,"boot":1,"type":"CLOSE","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1788409337,"uptime":37283,"boot":1,"type":"OPEN","detail":0,"rssi":-43},{"device":"petdoor-sample","epoch":1788409378,"uptime":37829,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1788409592,"uptime":38458,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788409642,"uptime":38608,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1788442686,"uptime":38660,"boot":1,"type":"OPEN","detail":0,"rssi":-43},{"device":"petdoor-sample","epoch":1788442733,"uptime":38893,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1788442987,"uptime":39232,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788443029,"uptime":39672,"boot":1,"type":"CLOSE","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1788456249,"uptime":40126,"boot":1,"type":"OPEN","detail":0,"rssi":-45},{"device":"petdoor-sample","epoch":1788456298,"uptime":40636,"boot":1,"type":"CLOSE","detail":0,"rssi":-70},{"device":"petdoor-sample","epoch":1788456642,"uptime":41213,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788456707,"uptime":41943,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1788472099,"uptime":42102,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1788472126,"uptime":42651,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1788475233,"uptime":43394,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788475309,"uptime":43975,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1788482403,"uptime":44261,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1788482453,"uptime":44602,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1788483473,"uptime":45364,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1788483526,"uptime":45846,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1788497235,"uptime":46548,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1788497300,"uptime":46954,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1788497487,"uptime":47124,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1788497571,"uptime":47353,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1788515355,"uptime":48227,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1788515358,"uptime":48534,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1788541113,"uptime":49101,"boot":1,"type":"OPEN","detail":0,"rssi":-42},{"device":"petdoor-sample","epoch":1788541170,"uptime":49850,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1788542047,"uptime":50330,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1788542126,"uptime":50765,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1788542176,"uptime":50924,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-87},{"device":"petdoor-sample","epoch":1788542180,"uptime":51339,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-88},{"device":"petdoor-sample","epoch":1788559677,"uptime":51888,"boot":1,"type":"OPEN","detail":0,"rssi":-44},{"device":"petdoor-sample","epoch":1788559731,"uptime":52272,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1788562800,"uptime":52523,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1788562865,"uptime":52648,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1788571856,"uptime":52967,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1788571922,"uptime":53366,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1788572357,"uptime":53739,"boot":1,"type":"OPEN","detail":0,"rssi":-45},{"device":"petdoor-sample","epoch":1788572431,"uptime":54384,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1788580819,"uptime":54624,"boot":1,"type":"OPEN","detail":0,"rssi":-49},{"device":"petdoor-sample","epoch":1788580878,"uptime":54787,"boot":1,"type":"CLOSE","detail":0,"rssi":-75},{"device":"petdoor-sample","epoch":1788581351,"uptime":54848,"boot":1,"type":"OPEN","detail":0,"rssi":-45},{"device":"petdoor-sample","epoch":1788581390,"uptime":55736,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1788620052,"uptime":55781,"boot":1,"type":"OPEN","detail":0,"rssi":-43},{"device":"petdoor-sample","epoch":1788620114,"uptime":56566,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1788623221,"uptime":57231,"boot":1,"type":"OPEN","detail":0,"rssi":-44},{"device":"petdoor-sample","epoch":1788623267,"uptime":57745,"boot":1,"type":"CLOSE","detail":0,"rssi":-75},{"device":"petdoor-sample","epoch":1788631366,"uptime":58344,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1788631424,"uptime":58375,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1788632293,"uptime":59223,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788632373,"uptime":60022,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1788635782,"uptime":60802,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1788635841,"uptime":61127,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1788637888,"uptime":61823,"boot":1,"type":"OPEN","detail":0,"rssi":-47},{"device":"petdoor-sample","epoch":1788637957,"uptime":62529,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1788643453,"uptime":62617,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788643479,"uptime":63510,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1788643665,"uptime":63751,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1788643721,"uptime":64342,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788659546,"uptime":65067,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1788659575,"uptime":65448,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1788660252,"uptime":66150,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1788660321,"uptime":66295,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1788672203,"uptime":67108,"boot":1,"type":"OPEN","detail":0,"rssi":-43},{"device":"petdoor-sample","epoch":1788672261,"uptime":67511,"boot":1,"type":"CLOSE","detail":0,"rssi":-75},{"device":"petdoor-sample","epoch":1788672702,"uptime":67771,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1788672761,"uptime":68593,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1788708519,"uptime":69260,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1788708575,"uptime":69545,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1788709622,"uptime":69657,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1788709694,"uptime":70495,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1788709920,"uptime":70952,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-84},{"device":"petdoor-sample","epoch":1788709921,"uptime":71094,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1788719418,"uptime":71515,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788719483,"uptime":72039,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788719949,"uptime":72120,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1788719997,"uptime":72641,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1788731865,"uptime":73053,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-85},{"device":"petdoor-sample","epoch":1788731867,"uptime":73409,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-86},{"device":"petdoor-sample","epoch":1788732936,"uptime":73720,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1788732975,"uptime":74249,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1788733334,"uptime":74989,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1788733395,"uptime":75295,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1788746534,"uptime":75541,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788746565,"uptime":75723,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1788748671,"uptime":76155,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1788748713,"uptime":76381,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1788757512,"uptime":76887,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1788757549,"uptime":77693,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788758081,"uptime":77883,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1788758123,"uptime":78262,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1788789972,"uptime":78963,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1788790034,"uptime":79712,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1788792662,"uptime":79977,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788792725,"uptime":80529,"boot":1,"type":"CLOSE","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1788802173,"uptime":80644,"boot":1,"type":"OPEN","detail":0,"rssi":-42},{"device":"petdoor-sample","epoch":1788802220,"uptime":81424,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788802909,"uptime":81794,"boot":1,"type":"OPEN","detail":0,"rssi":-47},{"device":"petdoor-sample","epoch":1788802968,"uptime":82246,"boot":1,"type":"CLOSE","detail":0,"rssi":-82},{"device":"petdoor-sample","epoch":1788809956,"uptime":83089,"boot":1,"type":"OPEN","detail":0,"rssi":-45},{"device":"petdoor-sample","epoch":1788810010,"uptime":83263,"boot":1,"type":"CLOSE","detail":0,"rssi":-75},{"device":"petdoor-sample","epoch":1788810311,"uptime":83772,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1788810364,"uptime":84388,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1788817025,"uptime":84671,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1788817095,"uptime":85409,"boot":1,"type":"CLOSE","detail":0,"rssi":-70},{"device":"petdoor-sample","epoch":1788817208,"uptime":86302,"boot":1,"type":"OPEN","detail":0,"rssi":-47},{"device":"petdoor-sample","epoch":1788817288,"uptime":86888,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1788829140,"uptime":87223,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1788829175,"uptime":87959,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1788831788,"uptime":88002,"boot":1,"type":"OPEN","detail":0,"rssi":-51},{"device":"petdoor-sample","epoch":1788831856,"uptime":88422,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1788841626,"uptime":88565,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788841662,"uptime":88958,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1788841936,"uptime":89042,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788842006,"uptime":89476,"boot":1,"type":"CLOSE","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1788874932,"uptime":89774,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788874993,"uptime":89849,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1788875327,"uptime":90255,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1788875391,"uptime":90782,"boot":1,"type":"CLOSE","detail":0,"rssi":-75},{"device":"petdoor-sample","epoch":1788889903,"uptime":91669,"boot":1,"type":"OPEN","detail":0,"rssi":-58},{"device":"petdoor-sample","epoch":1788889935,"uptime":92288,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1788890471,"uptime":92615,"boot":1,"type":"OPEN","detail":0,"rssi":-45},{"device":"petdoor-sample","epoch":1788890519,"uptime":93115,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1788896119,"uptime":93871,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-86},{"device":"petdoor-sample","epoch":1788896122,"uptime":94224,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1788902816,"uptime":94503,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1788902841,"uptime":94597,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1788903204,"uptime":94641,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1788903259,"uptime":94697,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1788918494,"uptime":95072,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1788918559,"uptime":95423,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1788919423,"uptime":95825,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1788919493,"uptime":96424,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1788928552,"uptime":96748,"boot":1,"type":"OPEN","detail":0,"rssi":-43},{"device":"petdoor-sample","epoch":1788928587,"uptime":97036,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1788928870,"uptime":97890,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1788928932,"uptime":98594,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1788955833,"uptime":98804,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-88},{"device":"petdoor-sample","epoch":1788955835,"uptime":99481,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-87},{"device":"petdoor-sample","epoch":1788972762,"uptime":99981,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1788972812,"uptime":100394,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1788973290,"uptime":101044,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1788973344,"uptime":101413,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1788989148,"uptime":101804,"boot":1,"type":"OPEN","detail":0,"rssi":-42},{"device":"petdoor-sample","epoch":1788989179,"uptime":101855,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1788989491,"uptime":102464,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1788989550,"uptime":103208,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1788996694,"uptime":103449,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1788996732,"uptime":104258,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1788997247,"uptime":104355,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1788997297,"uptime":105017,"boot":1,"type":"CLOSE","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1788998225,"uptime":105085,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-89},{"device":"petdoor-sample","epoch":1788998228,"uptime":105740,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1789002619,"uptime":106315,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1789002684,"uptime":106755,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789003170,"uptime":107402,"boot":1,"type":"OPEN","detail":0,"rssi":-51},{"device":"petdoor-sample","epoch":1789003246,"uptime":107847,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1789015675,"uptime":107932,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1789015732,"uptime":108234,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1789016049,"uptime":108323,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1789016107,"uptime":109192,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1789048114,"uptime":109687,"boot":1,"type":"OPEN","detail":0,"rssi":-51},{"device":"petdoor-sample","epoch":1789048179,"uptime":110167,"boot":1,"type":"CLOSE","detail":0,"rssi":-75},{"device":"petdoor-sample","epoch":1789050229,"uptime":110578,"boot":1,"type":"OPEN","detail":0,"rssi":-49},{"device":"petdoor-sample","epoch":1789050296,"uptime":110992,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789059666,"uptime":111051,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789059698,"uptime":111894,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1789060008,"uptime":112053,"boot":1,"type":"OPEN","detail":0,"rssi":-49},{"device":"petdoor-sample","epoch":1789060071,"uptime":112772,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789076195,"uptime":113311,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1789076238,"uptime":113610,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789076674,"uptime":113946,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1789076750,"uptime":114472,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1789090267,"uptime":114861,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789090319,"uptime":115365,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789090732,"uptime":115406,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1789090806,"uptime":115579,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789097031,"uptime":116218,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-88},{"device":"petdoor-sample","epoch":1789097034,"uptime":116435,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789101199,"uptime":116731,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789101268,"uptime":116984,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1789101612,"uptime":117268,"boot":1,"type":"OPEN","detail":0,"rssi":-45},{"device":"petdoor-sample","epoch":1789101701,"uptime":117984,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1789134545,"uptime":118229,"boot":1,"type":"OPEN","detail":0,"rssi":-51},{"device":"petdoor-sample","epoch":1789134595,"uptime":118489,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1789137673,"uptime":119042,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1789137733,"uptime":119062,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1789154551,"uptime":119638,"boot":1,"type":"OPEN","detail":0,"rssi":-47},{"device":"petdoor-sample","epoch":1789154600,"uptime":120061,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789154949,"uptime":120084,"boot":1,"type":"OPEN","detail":0,"rssi":-47},{"device":"petdoor-sample","epoch":1789155019,"uptime":120349,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1789163594,"uptime":120929,"boot":1,"type":"OPEN","detail":0,"rssi":-58},{"device":"petdoor-sample","epoch":1789163633,"uptime":121454,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789164059,"uptime":122343,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1789164100,"uptime":123028,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1789174028,"uptime":123831,"boot":1,"type":"OPEN","detail":0,"rssi":-49},{"device":"petdoor-sample","epoch":1789174087,"uptime":124412,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789174215,"uptime":124512,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789174218,"uptime":125171,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1789174557,"uptime":126034,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1789174614,"uptime":126628,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789185657,"uptime":127499,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1789185688,"uptime":128226,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1789185859,"uptime":129061,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1789185947,"uptime":129422,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789224124,"uptime":129712,"boot":1,"type":"OPEN","detail":0,"rssi":-58},{"device":"petdoor-sample","epoch":1789224173,"uptime":130086,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789224639,"uptime":130212,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1789224723,"uptime":130251,"boot":1,"type":"CLOSE","detail":0,"rssi":-82},{"device":"petdoor-sample","epoch":1789236456,"uptime":130591,"boot":1,"type":"OPEN","detail":0,"rssi":-45},{"device":"petdoor-sample","epoch":1789236490,"uptime":131435,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1789236712,"uptime":132188,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1789236771,"uptime":132420,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789249187,"uptime":133152,"boot":1,"type":"OPEN","detail":0,"rssi":-49},{"device":"petdoor-sample","epoch":1789249229,"uptime":133577,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1789249740,"uptime":134300,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1789249792,"uptime":134480,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789252291,"uptime":134696,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-93},{"device":"petdoor-sample","epoch":1789252295,"uptime":135560,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1789255988,"uptime":135744,"boot":1,"type":"OPEN","detail":0,"rssi":-57},{"device":"petdoor-sample","epoch":1789256042,"uptime":135927,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1789256326,"uptime":136288,"boot":1,"type":"OPEN","detail":0,"rssi":-44},{"device":"petdoor-sample","epoch":1789256373,"uptime":136944,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1789265888,"uptime":137669,"boot":1,"type":"OPEN","detail":0,"rssi":-51},{"device":"petdoor-sample","epoch":1789265951,"uptime":138191,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1789266265,"uptime":139035,"boot":1,"type":"OPEN","detail":0,"rssi":-47},{"device":"petdoor-sample","epoch":1789266342,"uptime":139632,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789276861,"uptime":139952,"boot":1,"type":"OPEN","detail":0,"rssi":-49},{"device":"petdoor-sample","epoch":1789276926,"uptime":140350,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789279972,"uptime":140794,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1789280048,"uptime":141106,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1789311074,"uptime":141501,"boot":1,"type":"OPEN","detail":0,"rssi":-58},{"device":"petdoor-sample","epoch":1789311129,"uptime":142287,"boot":1,"type":"CLOSE","detail":0,"rssi":-75},{"device":"petdoor-sample","epoch":1789311472,"uptime":142991,"boot":1,"type":"OPEN","detail":0,"rssi":-44},{"device":"petdoor-sample","epoch":1789311531,"uptime":143557,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789322971,"uptime":144087,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-92},{"device":"petdoor-sample","epoch":1789322975,"uptime":144349,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-88},{"device":"petdoor-sample","epoch":1789324849,"uptime":145030,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1789324883,"uptime":145305,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1789327970,"uptime":145695,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1789328043,"uptime":146047,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789328759,"uptime":146790,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1789328793,"uptime":146992,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789329052,"uptime":147538,"boot":1,"type":"OPEN","detail":0,"rssi":-51},{"device":"petdoor-sample","epoch":1789329101,"uptime":148313,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1789335961,"uptime":148462,"boot":1,"type":"OPEN","detail":0,"rssi":-44},{"device":"petdoor-sample","epoch":1789335991,"uptime":148546,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1789336711,"uptime":148636,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789336762,"uptime":149493,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1789349877,"uptime":149617,"boot":1,"type":"OPEN","detail":0,"rssi":-57},{"device":"petdoor-sample","epoch":1789349936,"uptime":150358,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1789350590,"uptime":150695,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1789350654,"uptime":150830,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1789354803,"uptime":151719,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1789354805,"uptime":152201,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789363166,"uptime":152745,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789363214,"uptime":153555,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789363606,"uptime":154270,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1789363682,"uptime":154782,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1789392485,"uptime":154843,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1789392544,"uptime":155029,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1789392931,"uptime":155685,"boot":1,"type":"OPEN","detail":0,"rssi":-51},{"device":"petdoor-sample","epoch":1789392995,"uptime":156127,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1789404998,"uptime":156640,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1789405044,"uptime":156714,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1789405369,"uptime":157086,"boot":1,"type":"OPEN","detail":0,"rssi":-47},{"device":"petdoor-sample","epoch":1789405437,"uptime":157365,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789421454,"uptime":157791,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1789421488,"uptime":158330,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789421991,"uptime":159107,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1789422067,"uptime":159942,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789438377,"uptime":160672,"boot":1,"type":"OPEN","detail":0,"rssi":-44},{"device":"petdoor-sample","epoch":1789438439,"uptime":161093,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1789438912,"uptime":161372,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1789438958,"uptime":162125,"boot":1,"type":"CLOSE","detail":0,"rssi":-82},{"device":"petdoor-sample","epoch":1789446464,"uptime":162795,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789446510,"uptime":163072,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1789447044,"uptime":163468,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1789447091,"uptime":164220,"boot":1,"type":"CLOSE","detail":0,"rssi":-70},{"device":"petdoor-sample","epoch":1789478025,"uptime":164939,"boot":1,"type":"OPEN","detail":0,"rssi":-58},{"device":"petdoor-sample","epoch":1789478066,"uptime":165293,"boot":1,"type":"CLOSE","detail":0,"rssi":-75},{"device":"petdoor-sample","epoch":1789479127,"uptime":165500,"boot":1,"type":"OPEN","detail":0,"rssi":-51},{"device":"petdoor-sample","epoch":1789479182,"uptime":165861,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789492448,"uptime":166757,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1789492491,"uptime":167554,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789492830,"uptime":167783,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1789492899,"uptime":168665,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789503609,"uptime":169262,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789503612,"uptime":170130,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-83},{"device":"petdoor-sample","epoch":1789507583,"uptime":170282,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1789507644,"uptime":170883,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1789508504,"uptime":171333,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1789508585,"uptime":172009,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789522333,"uptime":172752,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1789522363,"uptime":172900,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789522541,"uptime":173557,"boot":1,"type":"OPEN","detail":0,"rssi":-49},{"device":"petdoor-sample","epoch":1789522614,"uptime":174335,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1789531186,"uptime":175205,"boot":1,"type":"OPEN","detail":0,"rssi":-44},{"device":"petdoor-sample","epoch":1789531218,"uptime":175500,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1789531219,"uptime":176340,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-85},{"device":"petdoor-sample","epoch":1789531221,"uptime":176524,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-85},{"device":"petdoor-sample","epoch":1789531674,"uptime":176672,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1789531744,"uptime":176897,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1789566705,"uptime":177302,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789566759,"uptime":177958,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1789567803,"uptime":178663,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1789567887,"uptime":178981,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1789580188,"uptime":179237,"boot":1,"type":"OPEN","detail":0,"rssi":-58},{"device":"petdoor-sample","epoch":1789580223,"uptime":179924,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1789580749,"uptime":180133,"boot":1,"type":"OPEN","detail":0,"rssi":-49},{"device":"petdoor-sample","epoch":1789580794,"uptime":180167,"boot":1,"type":"CLOSE","detail":0,"rssi":-80},{"device":"petdoor-sample","epoch":1789590330,"uptime":180670,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-82},{"device":"petdoor-sample","epoch":1789590333,"uptime":181289,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1789594252,"uptime":182019,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1789594310,"uptime":182848,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789594790,"uptime":183004,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1789594864,"uptime":183298,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1789616022,"uptime":184022,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1789616026,"uptime":184319,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-75},{"device":"petdoor-sample","epoch":1789621234,"uptime":184768,"boot":1,"type":"OPEN","detail":0,"rssi":-43},{"device":"petdoor-sample","epoch":1789621297,"uptime":185046,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789621539,"uptime":185712,"boot":1,"type":"OPEN","detail":0,"rssi":-51},{"device":"petdoor-sample","epoch":1789621610,"uptime":186413,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1789650278,"uptime":186839,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-86},{"device":"petdoor-sample","epoch":1789650279,"uptime":187254,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1789663413,"uptime":187969,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1789663457,"uptime":188131,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789666564,"uptime":188200,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1789666603,"uptime":188381,"boot":1,"type":"CLOSE","detail":0,"rssi":-75},{"device":"petdoor-sample","epoch":1789672683,"uptime":189131,"boot":1,"type":"OPEN","detail":0,"rssi":-55},{"device":"petdoor-sample","epoch":1789672741,"uptime":189272,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1789674379,"uptime":189335,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1789674443,"uptime":189553,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789679086,"uptime":189865,"boot":1,"type":"OPEN","detail":0,"rssi":-45},{"device":"petdoor-sample","epoch":1789679147,"uptime":190250,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789681769,"uptime":191018,"boot":1,"type":"OPEN","detail":0,"rssi":-46},{"device":"petdoor-sample","epoch":1789681832,"uptime":191589,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1789694760,"uptime":192429,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1789694792,"uptime":192572,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789695691,"uptime":193006,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789695753,"uptime":193363,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789703162,"uptime":193410,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-86},{"device":"petdoor-sample","epoch":1789703165,"uptime":194282,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1789706844,"uptime":194857,"boot":1,"type":"OPEN","detail":0,"rssi":-44},{"device":"petdoor-sample","epoch":1789706897,"uptime":195753,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789707131,"uptime":196267,"boot":1,"type":"OPEN","detail":0,"rssi":-54},{"device":"petdoor-sample","epoch":1789707195,"uptime":196462,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789738901,"uptime":196864,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1789738960,"uptime":196952,"boot":1,"type":"CLOSE","detail":0,"rssi":-70},{"device":"petdoor-sample","epoch":1789740580,"uptime":197288,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1789740620,"uptime":197757,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789750130,"uptime":198596,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1789750181,"uptime":199474,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1789750658,"uptime":200159,"boot":1,"type":"OPEN","detail":0,"rssi":-51},{"device":"petdoor-sample","epoch":1789750736,"uptime":200469,"boot":1,"type":"CLOSE","detail":0,"rssi":-70},{"device":"petdoor-sample","epoch":1789761809,"uptime":200718,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-90},{"device":"petdoor-sample","epoch":1789761811,"uptime":201139,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-87},{"device":"petdoor-sample","epoch":1789767655,"uptime":201898,"boot":1,"type":"FIX_LOST","detail":0,"rssi":-83},{"device":"petdoor-sample","epoch":1789767656,"uptime":202550,"boot":1,"type":"FIX_GOT","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1789768399,"uptime":202987,"boot":1,"type":"OPEN","detail":0,"rssi":-49},{"device":"petdoor-sample","epoch":1789768431,"uptime":203790,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1789768803,"uptime":203955,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1789768850,"uptime":204565,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789774654,"uptime":204983,"boot":1,"type":"OPEN","detail":0,"rssi":-42},{"device":"petdoor-sample","epoch":1789774686,"uptime":205694,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789775085,"uptime":206424,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1789775133,"uptime":206665,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789779687,"uptime":207244,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789779740,"uptime":207553,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789779949,"uptime":208392,"boot":1,"type":"OPEN","detail":0,"rssi":-48},{"device":"petdoor-sample","epoch":1789779983,"uptime":208562,"boot":1,"type":"CLOSE","detail":0,"rssi":-82},{"device":"petdoor-sample","epoch":1789793369,"uptime":209034,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789793404,"uptime":209233,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1789793805,"uptime":210106,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1789793852,"uptime":210235,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789828054,"uptime":210824,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1789828086,"uptime":211101,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789830174,"uptime":211620,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1789830259,"uptime":212083,"boot":1,"type":"CLOSE","detail":0,"rssi":-75},{"device":"petdoor-sample","epoch":1789840085,"uptime":212226,"boot":1,"type":"OPEN","detail":0,"rssi":-43},{"device":"petdoor-sample","epoch":1789840144,"uptime":212401,"boot":1,"type":"CLOSE","detail":0,"rssi":-77},{"device":"petdoor-sample","epoch":1789840441,"uptime":212752,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1789840488,"uptime":212791,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789844500,"uptime":213087,"boot":1,"type":"OPEN","detail":0,"rssi":-53},{"device":"petdoor-sample","epoch":1789844545,"uptime":213686,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789845603,"uptime":214288,"boot":1,"type":"OPEN","detail":0,"rssi":-47},{"device":"petdoor-sample","epoch":1789845664,"uptime":214455,"boot":1,"type":"CLOSE","detail":0,"rssi":-70},{"device":"petdoor-sample","epoch":1789853858,"uptime":214755,"boot":1,"type":"OPEN","detail":0,"rssi":-58},{"device":"petdoor-sample","epoch":1789853887,"uptime":215415,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789854322,"uptime":215844,"boot":1,"type":"OPEN","detail":0,"rssi":-47},{"device":"petdoor-sample","epoch":1789854397,"uptime":216144,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1789871229,"uptime":216493,"boot":1,"type":"OPEN","detail":0,"rssi":-57},{"device":"petdoor-sample","epoch":1789871273,"uptime":216746,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1789872312,"uptime":216848,"boot":1,"type":"OPEN","detail":0,"rssi":-45},{"device":"petdoor-sample","epoch":1789872363,"uptime":217361,"boot":1,"type":"CLOSE","detail":0,"rssi":-73},{"device":"petdoor-sample","epoch":1789880757,"uptime":217586,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789880823,"uptime":217980,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789881858,"uptime":218564,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1789881909,"uptime":218849,"boot":1,"type":"CLOSE","detail":0,"rssi":-79},{"device":"petdoor-sample","epoch":1789917772,"uptime":219441,"boot":1,"type":"OPEN","detail":0,"rssi":-44},{"device":"petdoor-sample","epoch":1789917830,"uptime":219513,"boot":1,"type":"CLOSE","detail":0,"rssi":-69},{"device":"petdoor-sample","epoch":1789918075,"uptime":219688,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789918121,"uptime":219773,"boot":1,"type":"CLOSE","detail":0,"rssi":-76},{"device":"petdoor-sample","epoch":1789927754,"uptime":219800,"boot":1,"type":"OPEN","detail":0,"rssi":-49},{"device":"petdoor-sample","epoch":1789927809,"uptime":220598,"boot":1,"type":"CLOSE","detail":0,"rssi":-74},{"device":"petdoor-sample","epoch":1789928322,"uptime":220934,"boot":1,"type":"OPEN","detail":0,"rssi":-56},{"device":"petdoor-sample","epoch":1789928397,"uptime":221640,"boot":1,"type":"CLOSE","detail":0,"rssi":-71},{"device":"petdoor-sample","epoch":1789940937,"uptime":221740,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1789940969,"uptime":221764,"boot":1,"type":"CLOSE","detail":0,"rssi":-72},{"device":"petdoor-sample","epoch":1789941303,"uptime":221970,"boot":1,"type":"OPEN","detail":0,"rssi":-50},{"device":"petdoor-sample","epoch":1789941369,"uptime":222334,"boot":1,"type":"CLOSE","detail":0,"rssi":-81},{"device":"petdoor-sample","epoch":1789946922,"uptime":222849,"boot":1,"type":"OPEN","detail":0,"rssi":-52},{"device":"petdoor-sample","epoch":1789946949,"uptime":223012,"boot":1,"type":"CLOSE","detail":0,"rssi":-78},{"device":"petdoor-sample","epoch":1789947444,"uptime":223839,"boot":1,"type":"OPEN","detail":0,"rssi":-47},{"device":"petdoor-sample","epoch":1789947527,"uptime":224066,"boot":1,"type":"CLOSE","detail":0,"rssi":-82}],"devices":[{"device":"petdoor-sample","version":"1.1.0","build":"Sep 20 2026 17:30:00","boots":1,"last_seen":1789947527,"last_ip":"","status":"rssi=-61 raw=-63 dist=1.2 present=0 door=CLOSED gap=1840 samples=48210 adv=2104883 weak=61 heap=141208 up=186420","door_ip":"192.168.1.57","remote":1}],"commands":[{"device":"petdoor-sample","command":"thresholds -58 -68","queued":1789429127,"delivered":1789429367,"ack":"1 applied"},{"device":"petdoor-sample","command":"pulse 500","queued":1789601927,"delivered":1789602107,"ack":"1 applied"},{"device":"petdoor-sample","command":"openfilter 11 0.2","queued":1789774727,"delivered":1789775027,"ack":"0 applied, 1 refused: open filter rejected: must not be slower than the close filter"},{"device":"petdoor-sample","command":"dwell 1500 12000 5000","queued":1789861127,"delivered":1789861337,"ack":"1 applied"}]};
/* ---- data comes from the log server ---- */
let lastLoad=0;
async function load(){
  try{
    /* SAMPLE PAGE: synthetic data, baked in. This page never calls
       /api/events, which is the private route, so it can be public. */
    const j=window.__PETDOOR_SAMPLE__;
    events=(j.events||[]).map(e=>({epoch:+e.epoch||0,uptime:+e.uptime||0,boot:+e.boot||0,
      type:e.type,detail:+e.detail||0,rssi:+e.rssi||0,src:+e.src||0,device:e.device||""}));
    events.sort((a,b)=>(a.epoch||0)-(b.epoch||0)||a.boot-b.boot||a.uptime-b.uptime);
    commands=(j.commands||[]);
    renderDoors(j.devices||[]);
    lastLoad=Date.now();
    renderAll();
    const devs=[...new Set(events.map(e=>e.device).filter(Boolean))];
    $("live").textContent=events.length
      ? `${events.length} events${devs.length>1?` from ${devs.length} doors`:""} · updated just now`
      : "";
    if(!events.length){
      $("sub").innerHTML='No events yet. Point a door at this server &mdash; '
        +'set <code>LOG_ENDPOINT_URL</code> in its <code>secrets.h</code>.';
    }
  }catch(err){
    $("sub").textContent="Could not reach the log server ("+err.message+")";
    $("live").textContent="";
  }
}
$("refresh").onclick=load;
renderAll();
load();
// The door uploads in bursts when the pet is away, so there is no point polling
// hard. Once a minute keeps an always-on wall display current.
setInterval(load,60000);
setInterval(()=>{ if(lastLoad){const m=Math.round((Date.now()-lastLoad)/60000);
  if(m>=1&&events.length) $("live").textContent=$("live").textContent
    .replace(/updated .*/,`updated ${m} min ago`);} },60000);
